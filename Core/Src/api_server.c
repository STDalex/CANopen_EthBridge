#include "api_server.h"

#include "app_settings.h"
#include "app_version.h"
#include "can_bridge.h"
#include "can_log.h"
#include "can_pipeline.h"
#include "can_tx_manager.h"
#include "canopen_analyzer.h"
#include "canopen_decoder.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcp.h"
#include "main.h"
#include "memory_layout.h"
#include "network_service.h"
#include "sdo_history.h"
#include "task.h"
#include "web_assets.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_WRITE_BUFFER_BYTES       640U
#define API_FRAME_SNAPSHOT_MAX       128U
#define API_DEFAULT_PAGE_SIZE        32U
#define API_MAX_PAGE_SIZE            64U
#define WEBSOCKET_RING_CAPACITY      8192U
#define WEBSOCKET_BATCH_MAX_FRAMES   60U
#define WEBSOCKET_BATCH_HEADER_BYTES 16U
#define WEBSOCKET_FRAME_BYTES        24U
#define WEBSOCKET_PAYLOAD_MAX_BYTES \
  (WEBSOCKET_BATCH_HEADER_BYTES + \
   (WEBSOCKET_BATCH_MAX_FRAMES * WEBSOCKET_FRAME_BYTES))
#define WEBSOCKET_TX_MAX_BYTES       (4U + WEBSOCKET_PAYLOAD_MAX_BYTES)
#define WEBSOCKET_MAGIC              0x53575343UL /* "CSWS", little-endian. */
#define WEBSOCKET_PROTOCOL_VERSION   1U
#define WEBSOCKET_AGGREGATION_MS      5U
#define WEBSOCKET_AGGREGATION_POLL_MS 1U
#define WEBSOCKET_CONTROL_POLL_MS     20U
#define WEBSOCKET_DRAIN_BACKLOG       (2U * WEBSOCKET_BATCH_MAX_FRAMES)
#define WEBSOCKET_CRITICAL_BACKLOG    ((3U * WEBSOCKET_RING_CAPACITY) / 4U)
#define WEBSOCKET_BASE_PRIORITY       (tskIDLE_PRIORITY + 1U)
#define WEBSOCKET_DRAIN_PRIORITY      (tskIDLE_PRIORITY + 2U)
#define BRIDGE_WS_RX_BYTES           768U
#define BRIDGE_WS_TEXT_BYTES         512U
#define BRIDGE_PING_INTERVAL_MS      2000U
#define API_SNAPSHOT_BYTES \
  ((2U * API_FRAME_SNAPSHOT_MAX * sizeof(CanFrameRecord)) + \
   (CANOPEN_NODE_COUNT_MAX * sizeof(CanopenNodeSnapshot)) + \
   (CANOPEN_EVENT_SNAPSHOT_MAX * sizeof(CanopenNodeEventSnapshot)) + \
   (CAN_LOG_LIST_MAX * sizeof(CanLogFileInfo)))

_Static_assert(API_SNAPSHOT_BYTES <=
               (CAN_LOG_LIST_BUFFER_BASE - API_SNAPSHOT_BASE),
               "API snapshot region overlaps the CAN log list buffer");
_Static_assert((WEBSOCKET_RING_CAPACITY & (WEBSOCKET_RING_CAPACITY - 1U)) == 0U,
               "WebSocket ring capacity must be a power of two");
_Static_assert((WEBSOCKET_RING_CAPACITY * sizeof(CanFrameRecord)) <=
               WEBSOCKET_RING_REGION_BYTES,
               "WebSocket ring exceeds its external SDRAM region");
_Static_assert(WEBSOCKET_TX_MAX_BYTES <= 1460U,
               "WebSocket batch must fit one TCP MSS");

typedef struct
{
  struct netconn *connection;
  bool failed;
  char buffer[API_WRITE_BUFFER_BYTES];
} JsonWriter;

typedef enum
{
  API_REQUEST_OK = 0,
  API_REQUEST_INCOMPLETE,
  API_REQUEST_TOO_LARGE
} ApiRequestResult;

typedef struct
{
  uint32_t state[5];
  uint64_t total_bytes;
  uint8_t block[64];
  uint32_t block_bytes;
} Sha1Context;

typedef struct
{
  uint8_t data[BRIDGE_WS_RX_BYTES];
  size_t length;
} WebSocketReader;

typedef struct
{
  struct tcpip_api_call_data call;
  struct netconn *connection;
} TcpNoDelayCall;

static CanFrameRecord *const frame_history =
  (CanFrameRecord *)API_SNAPSHOT_BASE;
static uint32_t frame_history_write;
static uint32_t frame_history_count;
static CanFrameRecord *const frame_snapshot =
  (CanFrameRecord *)(API_SNAPSHOT_BASE +
    (API_FRAME_SNAPSHOT_MAX * sizeof(CanFrameRecord)));
static CanopenNodeSnapshot *const node_snapshot =
  (CanopenNodeSnapshot *)(API_SNAPSHOT_BASE +
    (2U * API_FRAME_SNAPSHOT_MAX * sizeof(CanFrameRecord)));
static CanopenNodeEventSnapshot *const event_snapshot =
  (CanopenNodeEventSnapshot *)(API_SNAPSHOT_BASE +
    (2U * API_FRAME_SNAPSHOT_MAX * sizeof(CanFrameRecord)) +
    (CANOPEN_NODE_COUNT_MAX * sizeof(CanopenNodeSnapshot)));
static CanLogFileInfo *const log_snapshot =
  (CanLogFileInfo *)(API_SNAPSHOT_BASE +
    (2U * API_FRAME_SNAPSHOT_MAX * sizeof(CanFrameRecord)) +
    (CANOPEN_NODE_COUNT_MAX * sizeof(CanopenNodeSnapshot)) +
    (CANOPEN_EVENT_SNAPSHOT_MAX * sizeof(CanopenNodeEventSnapshot)));
static CanFrameRecord *const websocket_ring =
  (CanFrameRecord *)WEBSOCKET_RING_BASE;
static char *const http_request_buffer =
  (char *)API_HTTP_REQUEST_BUFFER_BASE;
static volatile uint32_t websocket_write_sequence;
static StaticTask_t websocket_task_control;
static StackType_t *const websocket_task_stack =
  (StackType_t *)WEBSOCKET_TASK_STACK_BASE;
static TaskHandle_t websocket_task_handle;
static struct netconn *volatile websocket_pending_connection;
static volatile bool websocket_reserved;
static StaticTask_t bridge_task_control;
static StackType_t *const bridge_task_stack =
  (StackType_t *)BRIDGE_TASK_STACK_BASE;
static TaskHandle_t bridge_task_handle;
static struct netconn *volatile bridge_pending_connection;
static volatile bool bridge_websocket_reserved;
static volatile uint32_t bridge_pending_session_id;
static uint32_t bridge_stream_sequence;
static ApiServerStatus public_status;
static CanLogMode logger_configured_mode = CAN_LOG_MODE_FULL;
static CanTxHistoryEntry tx_history_snapshot[CAN_TX_HISTORY_CAPACITY];

static uint32_t QueryUnsigned(const char *query, const char *name,
                              uint32_t default_value, uint32_t maximum);
static bool JsonReadUnsigned(const char *json, const char *key,
                             uint32_t *value);
static bool JsonReadString(const char *json, const char *key,
                           char *value, size_t capacity);

static void StatusIncrement(uint32_t *value)
{
  taskENTER_CRITICAL();
  ++*value;
  taskEXIT_CRITICAL();
}

static void StatusAdd(uint32_t *value, uint32_t amount)
{
  taskENTER_CRITICAL();
  *value += amount;
  taskEXIT_CRITICAL();
}

static void StatusAdd64(uint64_t *value, uint64_t amount)
{
  taskENTER_CRITICAL();
  *value += amount;
  taskEXIT_CRITICAL();
}

static err_t EnableTcpNoDelayInCore(struct tcpip_api_call_data *call_data)
{
  TcpNoDelayCall *request = (TcpNoDelayCall *)call_data;
  if ((request == NULL) || (request->connection == NULL) ||
      (request->connection->pcb.tcp == NULL))
    return ERR_CONN;
  tcp_nagle_disable(request->connection->pcb.tcp);
  return ERR_OK;
}

static bool EnableTcpNoDelay(struct netconn *connection)
{
  TcpNoDelayCall request;
  memset(&request, 0, sizeof(request));
  request.connection = connection;
  return tcpip_api_call(EnableTcpNoDelayInCore, &request.call) == ERR_OK;
}

static uint32_t RotateLeft(uint32_t value, uint32_t bits)
{
  return (value << bits) | (value >> (32U - bits));
}

static void Sha1Transform(Sha1Context *context, const uint8_t block[64])
{
  uint32_t words[80];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t index;

  for (index = 0U; index < 16U; ++index)
  {
    const uint8_t *input = block + (index * 4U);
    words[index] = ((uint32_t)input[0] << 24) |
                   ((uint32_t)input[1] << 16) |
                   ((uint32_t)input[2] << 8) |
                   input[3];
  }
  for (index = 16U; index < 80U; ++index)
  {
    words[index] = RotateLeft(words[index - 3U] ^ words[index - 8U] ^
                              words[index - 14U] ^ words[index - 16U], 1U);
  }

  a = context->state[0];
  b = context->state[1];
  c = context->state[2];
  d = context->state[3];
  e = context->state[4];
  for (index = 0U; index < 80U; ++index)
  {
    uint32_t function;
    uint32_t constant;
    uint32_t temporary;
    if (index < 20U)
    {
      function = (b & c) | ((~b) & d);
      constant = 0x5A827999UL;
    }
    else if (index < 40U)
    {
      function = b ^ c ^ d;
      constant = 0x6ED9EBA1UL;
    }
    else if (index < 60U)
    {
      function = (b & c) | (b & d) | (c & d);
      constant = 0x8F1BBCDCUL;
    }
    else
    {
      function = b ^ c ^ d;
      constant = 0xCA62C1D6UL;
    }
    temporary = RotateLeft(a, 5U) + function + e + constant + words[index];
    e = d;
    d = c;
    c = RotateLeft(b, 30U);
    b = a;
    a = temporary;
  }
  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
}

static void Sha1Init(Sha1Context *context)
{
  context->state[0] = 0x67452301UL;
  context->state[1] = 0xEFCDAB89UL;
  context->state[2] = 0x98BADCFEUL;
  context->state[3] = 0x10325476UL;
  context->state[4] = 0xC3D2E1F0UL;
  context->total_bytes = 0U;
  context->block_bytes = 0U;
}

static void Sha1Update(Sha1Context *context, const uint8_t *data, size_t length)
{
  context->total_bytes += length;
  while (length > 0U)
  {
    size_t available = sizeof(context->block) - context->block_bytes;
    size_t copied = length < available ? length : available;
    memcpy(context->block + context->block_bytes, data, copied);
    context->block_bytes += (uint32_t)copied;
    data += copied;
    length -= copied;
    if (context->block_bytes == sizeof(context->block))
    {
      Sha1Transform(context, context->block);
      context->block_bytes = 0U;
    }
  }
}

static void Sha1Finish(Sha1Context *context, uint8_t digest[20])
{
  uint64_t total_bits = context->total_bytes * 8U;
  uint32_t index;
  context->block[context->block_bytes++] = 0x80U;
  if (context->block_bytes > 56U)
  {
    memset(context->block + context->block_bytes, 0,
           sizeof(context->block) - context->block_bytes);
    Sha1Transform(context, context->block);
    context->block_bytes = 0U;
  }
  memset(context->block + context->block_bytes, 0, 56U - context->block_bytes);
  for (index = 0U; index < 8U; ++index)
  {
    context->block[63U - index] = (uint8_t)(total_bits >> (index * 8U));
  }
  Sha1Transform(context, context->block);
  for (index = 0U; index < 5U; ++index)
  {
    digest[index * 4U] = (uint8_t)(context->state[index] >> 24);
    digest[index * 4U + 1U] = (uint8_t)(context->state[index] >> 16);
    digest[index * 4U + 2U] = (uint8_t)(context->state[index] >> 8);
    digest[index * 4U + 3U] = (uint8_t)context->state[index];
  }
}

static void Base64Encode20(const uint8_t input[20], char output[29])
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t input_index = 0U;
  uint32_t output_index = 0U;
  while (input_index + 3U <= 20U)
  {
    uint32_t value = ((uint32_t)input[input_index] << 16) |
                     ((uint32_t)input[input_index + 1U] << 8) |
                     input[input_index + 2U];
    output[output_index++] = alphabet[(value >> 18) & 0x3FU];
    output[output_index++] = alphabet[(value >> 12) & 0x3FU];
    output[output_index++] = alphabet[(value >> 6) & 0x3FU];
    output[output_index++] = alphabet[value & 0x3FU];
    input_index += 3U;
  }
  {
    uint32_t value = ((uint32_t)input[input_index] << 16) |
                     ((uint32_t)input[input_index + 1U] << 8);
    output[output_index++] = alphabet[(value >> 18) & 0x3FU];
    output[output_index++] = alphabet[(value >> 12) & 0x3FU];
    output[output_index++] = alphabet[(value >> 6) & 0x3FU];
    output[output_index++] = '=';
  }
  output[output_index] = '\0';
}

static bool WriteRaw(struct netconn *connection, const void *data, size_t length)
{
  const uint8_t *cursor = (const uint8_t *)data;

  while (length > 0U)
  {
    size_t written = 0U;
    err_t error = netconn_write_partly(connection, cursor, length,
                                       NETCONN_COPY, &written);
    if ((error != ERR_OK) || (written == 0U))
    {
      return false;
    }
    cursor += written;
    length -= written;
  }
  return true;
}

static void WriterBegin(JsonWriter *writer, struct netconn *connection,
                        const char *status)
{
  char header[256];
  int length;

  writer->connection = connection;
  writer->failed = false;
  length = snprintf(header, sizeof(header),
                    "HTTP/1.1 %s\r\n"
                    "Content-Type: application/json; charset=utf-8\r\n"
                    "Cache-Control: no-store\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n", status);
  if ((length <= 0) || ((size_t)length >= sizeof(header)) ||
      !WriteRaw(connection, header, (size_t)length))
  {
    writer->failed = true;
  }
}

static void WriterChunk(JsonWriter *writer, const char *data, size_t length)
{
  char prefix[16];
  int prefix_length;

  if (writer->failed || (length == 0U))
  {
    return;
  }
  prefix_length = snprintf(prefix, sizeof(prefix), "%X\r\n", (unsigned int)length);
  if ((prefix_length <= 0) || !WriteRaw(writer->connection, prefix, (size_t)prefix_length) ||
      !WriteRaw(writer->connection, data, length) ||
      !WriteRaw(writer->connection, "\r\n", 2U))
  {
    writer->failed = true;
  }
}

static void WriterPrintf(JsonWriter *writer, const char *format, ...)
{
  va_list arguments;
  int length;

  if (writer->failed)
  {
    return;
  }
  va_start(arguments, format);
  length = vsnprintf(writer->buffer, sizeof(writer->buffer), format, arguments);
  va_end(arguments);
  if ((length < 0) || ((size_t)length >= sizeof(writer->buffer)))
  {
    writer->failed = true;
    return;
  }
  WriterChunk(writer, writer->buffer, (size_t)length);
}

static void WriterEnd(JsonWriter *writer)
{
  if (!writer->failed && !WriteRaw(writer->connection, "0\r\n\r\n", 5U))
  {
    writer->failed = true;
  }
  if (writer->failed)
  {
    StatusIncrement(&public_status.send_errors);
  }
}

static void SendError(struct netconn *connection, const char *status,
                      const char *code, const char *message)
{
  JsonWriter writer;
  WriterBegin(&writer, connection, status);
  WriterPrintf(&writer, "{\"error\":\"%s\",\"message\":\"%s\"}\n",
               code, message);
  WriterEnd(&writer);
}

static char AsciiLower(char character)
{
  if ((character >= 'A') && (character <= 'Z'))
  {
    return (char)(character + ('a' - 'A'));
  }
  return character;
}

static bool AsciiEquals(const char *left, size_t left_length,
                        const char *right)
{
  size_t index;
  if (left_length != strlen(right))
  {
    return false;
  }
  for (index = 0U; index < left_length; ++index)
  {
    if (AsciiLower(left[index]) != AsciiLower(right[index]))
    {
      return false;
    }
  }
  return true;
}

static bool FindHeaderValue(const char *request, const char *name,
                            char *value, size_t capacity)
{
  const char *line = strstr(request, "\r\n");
  if ((line == NULL) || (capacity == 0U))
  {
    return false;
  }
  line += 2;
  while ((line[0] != '\r') || (line[1] != '\n'))
  {
    const char *line_end = strstr(line, "\r\n");
    const char *colon;
    const char *start;
    const char *end;
    size_t length;
    if (line_end == NULL)
    {
      return false;
    }
    colon = memchr(line, ':', (size_t)(line_end - line));
    if ((colon != NULL) && AsciiEquals(line, (size_t)(colon - line), name))
    {
      start = colon + 1;
      while ((start < line_end) && ((*start == ' ') || (*start == '\t')))
      {
        ++start;
      }
      end = line_end;
      while ((end > start) && ((end[-1] == ' ') || (end[-1] == '\t')))
      {
        --end;
      }
      length = (size_t)(end - start);
      if (length >= capacity)
      {
        return false;
      }
      memcpy(value, start, length);
      value[length] = '\0';
      return true;
    }
    line = line_end + 2;
  }
  return false;
}

static bool HeaderHasToken(const char *value, const char *token)
{
  size_t token_length = strlen(token);
  const char *cursor = value;
  while (*cursor != '\0')
  {
    const char *start;
    const char *end;
    while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == ','))
    {
      ++cursor;
    }
    start = cursor;
    while ((*cursor != '\0') && (*cursor != ','))
    {
      ++cursor;
    }
    end = cursor;
    while ((end > start) && ((end[-1] == ' ') || (end[-1] == '\t')))
    {
      --end;
    }
    if (((size_t)(end - start) == token_length) &&
        AsciiEquals(start, token_length, token))
    {
      return true;
    }
  }
  return false;
}

static const char *WebContentType(WebAssetContentType type)
{
  switch (type)
  {
    case WEB_ASSET_CONTENT_HTML: return "text/html; charset=utf-8";
    case WEB_ASSET_CONTENT_JAVASCRIPT: return "text/javascript; charset=utf-8";
    case WEB_ASSET_CONTENT_CSS: return "text/css; charset=utf-8";
    default: return "application/octet-stream";
  }
}

static void HandleWebAsset(struct netconn *connection, const char *request,
                           const char *requested_path, bool send_body)
{
  WebAsset asset;
  const char *path = strcmp(requested_path, "/") == 0 ?
                     "/index.html" : requested_path;
  char header[512];
  char if_none_match[40];
  char expected_etag[24];
  uint8_t buffer[1024];
  uint32_t offset = 0U;
  int length;

  if (!WebAssets_Find(path, &asset))
  {
    SendError(connection, "404 Not Found", "web_asset_not_found",
              "Web asset is not installed in QSPI");
    return;
  }
  (void)snprintf(expected_etag, sizeof(expected_etag), "\"%s\"", asset.etag);
  if (FindHeaderValue(request, "If-None-Match", if_none_match,
                      sizeof(if_none_match)) &&
      (strcmp(if_none_match, expected_etag) == 0))
  {
    length = snprintf(header, sizeof(header),
      "HTTP/1.1 304 Not Modified\r\n"
      "ETag: %s\r\n"
      "Cache-Control: %s\r\n"
      "Connection: close\r\n\r\n",
      expected_etag, asset.immutable ?
      "public, max-age=31536000, immutable" : "no-cache");
    if ((length <= 0) || ((size_t)length >= sizeof(header)) ||
        !WriteRaw(connection, header, (size_t)length))
    {
      StatusIncrement(&public_status.send_errors);
    }
    return;
  }

  length = snprintf(header, sizeof(header),
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: %s\r\n"
    "Content-Encoding: gzip\r\n"
    "Content-Length: %lu\r\n"
    "ETag: %s\r\n"
    "Cache-Control: %s\r\n"
    "Vary: Accept-Encoding\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: close\r\n\r\n",
    WebContentType(asset.content_type),
    (unsigned long)asset.compressed_size, expected_etag,
    asset.immutable ? "public, max-age=31536000, immutable" : "no-cache");
  if ((length <= 0) || ((size_t)length >= sizeof(header)) ||
      !WriteRaw(connection, header, (size_t)length))
  {
    StatusIncrement(&public_status.send_errors);
    return;
  }
  while (send_body && (offset < asset.compressed_size))
  {
    uint32_t chunk = asset.compressed_size - offset;
    if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
    if (!WebAssets_Read(&asset, offset, buffer, chunk) ||
        !WriteRaw(connection, buffer, chunk))
    {
      StatusIncrement(&public_status.send_errors);
      return;
    }
    offset += chunk;
  }
}

static void StoreLe16(uint8_t *destination, uint16_t value)
{
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8);
}

static void StoreLe32(uint8_t *destination, uint32_t value)
{
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8);
  destination[2] = (uint8_t)(value >> 16);
  destination[3] = (uint8_t)(value >> 24);
}

static void StoreLe64(uint8_t *destination, uint64_t value)
{
  StoreLe32(destination, (uint32_t)value);
  StoreLe32(destination + 4U, (uint32_t)(value >> 32));
}

static bool WebSocketSendFrame(struct netconn *connection, uint8_t opcode,
                               const uint8_t *payload, size_t payload_length)
{
  uint8_t header[4];
  struct netvector vectors[2];
  size_t header_length;
  size_t written = 0U;
  size_t total_length;
  err_t error;
  header[0] = (uint8_t)(0x80U | opcode);
  if (payload_length <= 125U)
  {
    header[1] = (uint8_t)payload_length;
    header_length = 2U;
  }
  else if (payload_length <= 65535U)
  {
    header[1] = 126U;
    header[2] = (uint8_t)(payload_length >> 8);
    header[3] = (uint8_t)payload_length;
    header_length = 4U;
  }
  else
  {
    return false;
  }

  /* Submit the WebSocket header and payload in one lwIP API operation. Two
     separate netconn_write() calls let the tiny header leave as its own TCP
     segment; Nagle then holds the payload until the peer's delayed ACK (about
     40-50 ms on Windows), effectively limiting Bridge SDO traffic to 20/s. */
  vectors[0].ptr = header;
  vectors[0].len = header_length;
  vectors[1].ptr = payload;
  vectors[1].len = payload_length;
  total_length = header_length + payload_length;
  error = netconn_write_vectors_partly(connection, vectors,
                                        payload_length == 0U ? 1U : 2U,
                                        NETCONN_COPY, &written);
  return (error == ERR_OK) && (written == total_length);
}

static bool WebSocketReadControl(struct netconn *connection)
{
  struct netbuf *buffer = NULL;
  uint8_t packet[136];
  uint8_t payload[125];
  uint16_t length;
  size_t header_length;
  size_t payload_length;
  size_t index;
  uint8_t opcode;
  err_t error = netconn_recv(connection, &buffer);

  if (error == ERR_TIMEOUT)
  {
    return true;
  }
  if ((error != ERR_OK) || (buffer == NULL))
  {
    if (buffer != NULL) netbuf_delete(buffer);
    return false;
  }
  length = netbuf_len(buffer);
  if ((length < 2U) || (length > sizeof(packet)))
  {
    netbuf_delete(buffer);
    return false;
  }
  (void)netbuf_copy(buffer, packet, length);
  netbuf_delete(buffer);

  opcode = packet[0] & 0x0FU;
  payload_length = packet[1] & 0x7FU;
  if (((packet[0] & 0x80U) == 0U) || ((packet[1] & 0x80U) == 0U) ||
      (payload_length > 125U))
  {
    return false;
  }
  header_length = 6U;
  if ((size_t)length < header_length + payload_length)
  {
    return false;
  }
  for (index = 0U; index < payload_length; ++index)
  {
    payload[index] = packet[header_length + index] ^ packet[2U + (index & 3U)];
  }
  if (opcode == 0x08U)
  {
    (void)WebSocketSendFrame(connection, 0x08U, payload, payload_length);
    return false;
  }
  if (opcode == 0x09U)
  {
    return WebSocketSendFrame(connection, 0x0AU, payload, payload_length);
  }
  return true;
}

/* Returns 1 for a complete frame, 0 when more TCP data is needed, and -1 on
   disconnect or malformed input. Client-to-server frames must be masked. */
static int WebSocketReadFrame(struct netconn *connection,
                              WebSocketReader *reader,
                              uint8_t *opcode,
                              uint8_t *payload,
                              size_t payload_capacity,
                              size_t *payload_length)
{
  size_t header_length;
  size_t frame_length;
  size_t length;
  uint8_t mask[4];
  size_t index;

  for (;;)
  {
    if (reader->length >= 2U)
    {
      uint8_t length_code = reader->data[1] & 0x7FU;
      if (((reader->data[0] & 0x80U) == 0U) ||
          ((reader->data[1] & 0x80U) == 0U)) return -1;
      if (length_code <= 125U)
      {
        length = length_code;
        header_length = 6U;
      }
      else if (length_code == 126U)
      {
        if (reader->length < 4U) goto receive_more;
        length = ((size_t)reader->data[2] << 8U) | reader->data[3];
        header_length = 8U;
      }
      else return -1;
      if (length > payload_capacity) return -1;
      frame_length = header_length + length;
      if (reader->length >= frame_length)
      {
        memcpy(mask, reader->data + header_length - 4U, sizeof(mask));
        for (index = 0U; index < length; ++index)
          payload[index] = reader->data[header_length + index] ^ mask[index & 3U];
        *opcode = reader->data[0] & 0x0FU;
        *payload_length = length;
        reader->length -= frame_length;
        if (reader->length != 0U)
          memmove(reader->data, reader->data + frame_length, reader->length);
        return 1;
      }
    }

receive_more:
    {
      struct netbuf *buffer = NULL;
      err_t error = netconn_recv(connection, &buffer);
      uint16_t received;
      if (error == ERR_TIMEOUT) return 0;
      if ((error != ERR_OK) || (buffer == NULL))
      {
        if (buffer != NULL) netbuf_delete(buffer);
        return -1;
      }
      received = netbuf_len(buffer);
      if ((received == 0U) ||
          ((reader->length + received) > sizeof(reader->data)))
      {
        netbuf_delete(buffer);
        return -1;
      }
      (void)netbuf_copy(buffer, reader->data + reader->length, received);
      reader->length += received;
      netbuf_delete(buffer);
    }
  }
}

static uint32_t WebSocketCopyBatch(uint32_t *sequence, uint32_t *lost,
                                   CanFrameRecord *frames,
                                   uint32_t *remaining)
{
  uint32_t available;
  uint32_t count;
  uint32_t index;
  taskENTER_CRITICAL();
  available = websocket_write_sequence - *sequence;
  if (available > WEBSOCKET_RING_CAPACITY)
  {
    uint32_t skipped = available - WEBSOCKET_RING_CAPACITY;
    *sequence += skipped;
    *lost += skipped;
    available = WEBSOCKET_RING_CAPACITY;
  }
  count = available > WEBSOCKET_BATCH_MAX_FRAMES ?
          WEBSOCKET_BATCH_MAX_FRAMES : available;
  for (index = 0U; index < count; ++index)
  {
    frames[index] = websocket_ring[(*sequence + index) &
                                    (WEBSOCKET_RING_CAPACITY - 1U)];
  }
  if (remaining != NULL)
  {
    *remaining = available - count;
  }
  taskEXIT_CRITICAL();
  return count;
}

static size_t WebSocketBuildBatch(uint8_t *payload, const CanFrameRecord *frames,
                                  uint32_t count, uint32_t sequence,
                                  uint32_t lost)
{
  uint32_t index;
  StoreLe32(payload, WEBSOCKET_MAGIC);
  payload[4] = WEBSOCKET_PROTOCOL_VERSION;
  payload[5] = lost == 0U ? 0U : 1U;
  StoreLe16(payload + 6U, (uint16_t)count);
  StoreLe32(payload + 8U, sequence);
  StoreLe32(payload + 12U, lost);
  for (index = 0U; index < count; ++index)
  {
    const CanFrameRecord *frame = &frames[index];
    uint8_t *record = payload + WEBSOCKET_BATCH_HEADER_BYTES +
                      (index * WEBSOCKET_FRAME_BYTES);
    uint64_t timestamp = ((uint64_t)frame->capture_time_high << 32) |
                         frame->capture_time_us;
    StoreLe64(record, timestamp);
    StoreLe32(record + 8U, frame->id);
    record[12] = frame->flags;
    record[13] = frame->dlc;
    record[14] = 0U;
    record[15] = 0U;
    memcpy(record + 16U, frame->data, 8U);
  }
  return WEBSOCKET_BATCH_HEADER_BYTES + (count * WEBSOCKET_FRAME_BYTES);
}

static void WebSocketLiveRun(struct netconn *connection)
{
  CanFrameRecord frames[WEBSOCKET_BATCH_MAX_FRAMES];
  uint8_t transmit[WEBSOCKET_TX_MAX_BYTES];
  char hello[128];
  uint32_t sequence;
  uint32_t lost = 0U;
  uint32_t aggregation_started_tick = 0U;
  uint32_t last_control_tick = 0U;
  bool drain_mode = false;
  bool priority_boosted = false;
  int hello_length;

  taskENTER_CRITICAL();
  sequence = websocket_write_sequence;
  taskEXIT_CRITICAL();
  hello_length = snprintf(hello, sizeof(hello),
    "{\"type\":\"hello\",\"protocol\":%u,\"mode\":\"live\"," 
    "\"record_bytes\":%u,\"sequence\":%lu}",
    (unsigned int)WEBSOCKET_PROTOCOL_VERSION,
    (unsigned int)WEBSOCKET_FRAME_BYTES, (unsigned long)sequence);
  if ((hello_length <= 0) || ((size_t)hello_length >= sizeof(hello)) ||
      !WebSocketSendFrame(connection, 0x01U, (const uint8_t *)hello,
                          (size_t)hello_length))
  {
    StatusIncrement(&public_status.send_errors);
    return;
  }

  netconn_set_recvtimeout(connection, 1);
  netconn_set_sendtimeout(connection, 1000);
  last_control_tick = HAL_GetTick();
  for (;;)
  {
    uint32_t count;
    uint32_t remaining = 0U;
    uint32_t backlog;
    uint32_t now = HAL_GetTick();
    size_t payload_length;
    if ((!drain_mode ||
         ((uint32_t)(now - last_control_tick) >= WEBSOCKET_CONTROL_POLL_MS)) &&
        !WebSocketReadControl(connection))
    {
      break;
    }
    if (!drain_mode ||
        ((uint32_t)(now - last_control_tick) >= WEBSOCKET_CONTROL_POLL_MS))
    {
      last_control_tick = HAL_GetTick();
    }
    count = WebSocketCopyBatch(&sequence, &lost, frames, &remaining);
    if (count == 0U)
    {
      aggregation_started_tick = 0U;
      drain_mode = false;
      if (priority_boosted)
      {
        vTaskPrioritySet(NULL, WEBSOCKET_BASE_PRIORITY);
        priority_boosted = false;
      }
      vTaskDelay(pdMS_TO_TICKS(WEBSOCKET_AGGREGATION_MS));
      continue;
    }
    backlog = count + remaining;
    drain_mode = backlog >= WEBSOCKET_DRAIN_BACKLOG;

    /* With a light stream, wait briefly so several CAN frames share one TCP
       segment. Once one complete batch is queued, start draining immediately. */
    if ((count < WEBSOCKET_BATCH_MAX_FRAMES) && !drain_mode)
    {
      now = HAL_GetTick();
      if (aggregation_started_tick == 0U)
      {
        aggregation_started_tick = now;
      }
      if ((uint32_t)(now - aggregation_started_tick) <
          WEBSOCKET_AGGREGATION_MS)
      {
        vTaskDelay(pdMS_TO_TICKS(WEBSOCKET_AGGREGATION_POLL_MS));
        continue;
      }
    }
    aggregation_started_tick = 0U;

    if ((backlog >= WEBSOCKET_CRITICAL_BACKLOG) && !priority_boosted)
    {
      vTaskPrioritySet(NULL, WEBSOCKET_DRAIN_PRIORITY);
      priority_boosted = true;
    }
    else if ((backlog < WEBSOCKET_DRAIN_BACKLOG) && priority_boosted)
    {
      vTaskPrioritySet(NULL, WEBSOCKET_BASE_PRIORITY);
      priority_boosted = false;
    }
    payload_length = WebSocketBuildBatch(transmit + 4U, frames, count,
                                          sequence, lost);
    transmit[0] = 0x82U;
    if (payload_length <= 125U)
    {
      /* RFC 6455 requires the shortest possible length encoding. */
      memmove(transmit + 2U, transmit + 4U, payload_length);
      transmit[1] = (uint8_t)payload_length;
      payload_length += 2U;
    }
    else
    {
      transmit[1] = 126U;
      transmit[2] = (uint8_t)(payload_length >> 8);
      transmit[3] = (uint8_t)payload_length;
      payload_length += 4U;
    }
    if (!WriteRaw(connection, transmit, payload_length))
    {
      StatusIncrement(&public_status.send_errors);
      break;
    }
    if (lost != 0U)
    {
      StatusAdd(&public_status.websocket_lost_frames, lost);
      lost = 0U;
    }
    sequence += count;
    StatusIncrement(&public_status.websocket_batches);
    StatusAdd(&public_status.websocket_frames, count);
  }
  if (priority_boosted)
  {
    vTaskPrioritySet(NULL, WEBSOCKET_BASE_PRIORITY);
  }
}

static void WebSocketTask(void *argument)
{
  (void)argument;
  for (;;)
  {
    struct netconn *connection;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    taskENTER_CRITICAL();
    connection = websocket_pending_connection;
    websocket_pending_connection = NULL;
    taskEXIT_CRITICAL();
    if (connection != NULL)
    {
      WebSocketLiveRun(connection);
      (void)netconn_close(connection);
      netconn_delete(connection);
    }
    taskENTER_CRITICAL();
    websocket_reserved = false;
    public_status.websocket_active = false;
    taskEXIT_CRITICAL();
  }
}

static bool WebSocketAccept(struct netconn *connection, const char *request)
{
  static const char websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char upgrade[32];
  char connection_header[64];
  char version[8];
  char key[64];
  char source[128];
  char accept[29];
  char response[256];
  uint8_t digest[20];
  Sha1Context sha1;
  int source_length;
  int response_length;

  if (!FindHeaderValue(request, "Upgrade", upgrade, sizeof(upgrade)) ||
      !HeaderHasToken(upgrade, "websocket") ||
      !FindHeaderValue(request, "Connection", connection_header,
                       sizeof(connection_header)) ||
      !HeaderHasToken(connection_header, "Upgrade") ||
      !FindHeaderValue(request, "Sec-WebSocket-Version", version,
                       sizeof(version)) ||
      (strcmp(version, "13") != 0) ||
      !FindHeaderValue(request, "Sec-WebSocket-Key", key, sizeof(key)))
  {
    StatusIncrement(&public_status.bad_requests);
    SendError(connection, "400 Bad Request", "bad_websocket_upgrade",
              "RFC 6455 WebSocket upgrade is required");
    return false;
  }

  source_length = snprintf(source, sizeof(source), "%s%s", key, websocket_guid);
  if ((source_length <= 0) || ((size_t)source_length >= sizeof(source)))
  {
    SendError(connection, "400 Bad Request", "bad_websocket_key",
              "Sec-WebSocket-Key is too long");
    return false;
  }
  Sha1Init(&sha1);
  Sha1Update(&sha1, (const uint8_t *)source, (size_t)source_length);
  Sha1Finish(&sha1, digest);
  Base64Encode20(digest, accept);
  response_length = snprintf(response, sizeof(response),
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
  if ((response_length <= 0) || ((size_t)response_length >= sizeof(response)) ||
      !WriteRaw(connection, response, (size_t)response_length))
  {
    StatusIncrement(&public_status.send_errors);
    return false;
  }

  return true;
}

static bool HandleWebSocketUpgrade(struct netconn *connection,
                                   const char *request)
{
  bool available;

  taskENTER_CRITICAL();
  available = !websocket_reserved;
  if (available)
  {
    websocket_reserved = true;
    public_status.websocket_active = true;
  }
  taskEXIT_CRITICAL();
  if (!available)
  {
    SendError(connection, "503 Service Unavailable", "websocket_busy",
              "The live stream already has a client");
    return false;
  }
  if (!WebSocketAccept(connection, request))
  {
    taskENTER_CRITICAL();
    websocket_reserved = false;
    public_status.websocket_active = false;
    taskEXIT_CRITICAL();
    return false;
  }

  taskENTER_CRITICAL();
  websocket_pending_connection = connection;
  ++public_status.websocket_connections;
  taskEXIT_CRITICAL();
  xTaskNotifyGive(websocket_task_handle);
  return true;
}

static const char *BridgeSubmitName(CanBridgeSubmitResult result)
{
  switch (result)
  {
    case CAN_BRIDGE_SUBMIT_OK: return "accepted";
    case CAN_BRIDGE_SUBMIT_DUPLICATE: return "duplicate";
    case CAN_BRIDGE_SUBMIT_DISABLED: return "bridge_disabled";
    case CAN_BRIDGE_SUBMIT_NOT_CONNECTED: return "not_connected";
    case CAN_BRIDGE_SUBMIT_BUSY: return "request_pending";
    case CAN_BRIDGE_SUBMIT_INVALID: return "invalid_request";
    default: return "can_not_ready";
  }
}

static bool BridgeSendText(struct netconn *connection, const char *text)
{
  return WebSocketSendFrame(connection, 0x01U, (const uint8_t *)text,
                            strlen(text));
}

static bool BridgeSendResult(struct netconn *connection,
                             const CanBridgeResult *result)
{
  char text[384];
  char raw[17];
  uint32_t index;
  int length;
  for (index = 0U; index < 8U; ++index)
    (void)snprintf(raw + (index * 2U), 3U, "%02X",
                   (unsigned int)result->response.data[index]);
  raw[16] = '\0';
  length = snprintf(text, sizeof(text),
    "{\"type\":\"result\",\"request_id\":%lu,\"tx_sequence\":%lu,"
    "\"status\":\"%s\",\"timestamp_us\":%lu,\"capture_time_us\":%llu,"
    "\"abort_code\":%lu,"
    "\"has_response\":%s,\"can_id\":%lu,\"dlc\":%u,\"data\":\"%s\"}",
    (unsigned long)result->client_request_id,
    (unsigned long)result->tx_sequence,
    CanBridge_ResultName(result->code),
    (unsigned long)result->timestamp_us,
    (unsigned long long)result->capture_time_us,
    (unsigned long)result->abort_code,
    result->has_response ? "true" : "false",
    (unsigned long)result->response.id,
    (unsigned int)result->response.dlc, raw);
  return (length > 0) && ((size_t)length < sizeof(text)) &&
         WebSocketSendFrame(connection, 0x01U, (const uint8_t *)text,
                            (size_t)length);
}

static bool BridgeParseSdo(const char *json, uint32_t *request_id,
                           CanTxSdoRequest *request)
{
  uint32_t value;
  char command[16];
  CanTxFrame validation_frame;
  memset(request, 0, sizeof(*request));
  request->timeout_ms = CAN_TX_DEFAULT_TIMEOUT_MS;
  if (!JsonReadUnsigned(json, "request_id", request_id) ||
      (*request_id == 0U) ||
      !JsonReadUnsigned(json, "node_id", &value) ||
      (value < 1U) || (value > 127U)) return false;
  request->node_id = (uint8_t)value;
  if (!JsonReadUnsigned(json, "index", &value) || (value > 0xFFFFU))
    return false;
  request->index = (uint16_t)value;
  if (!JsonReadUnsigned(json, "subindex", &value) || (value > 0xFFU))
    return false;
  request->subindex = (uint8_t)value;
  if (!JsonReadString(json, "command", command, sizeof(command))) return false;
  if (strcmp(command, "read") == 0) request->command = CAN_TX_SDO_READ;
  else if (strcmp(command, "write_u8") == 0) request->command = CAN_TX_SDO_WRITE_U8;
  else if (strcmp(command, "write_u16") == 0) request->command = CAN_TX_SDO_WRITE_U16;
  else if (strcmp(command, "write_u24") == 0) request->command = CAN_TX_SDO_WRITE_U24;
  else if (strcmp(command, "write_u32") == 0) request->command = CAN_TX_SDO_WRITE_U32;
  else return false;
  if ((request->command != CAN_TX_SDO_READ) &&
      !JsonReadUnsigned(json, "value", &request->value)) return false;
  if (JsonReadUnsigned(json, "timeout_ms", &value))
  {
    if ((value < 10U) || (value > 1000U)) return false;
    request->timeout_ms = (uint16_t)value;
  }
  return CanTx_BuildSdoRequest(request, &validation_frame);
}

static bool BridgeHandleText(struct netconn *connection, uint32_t session_id,
                             char *json)
{
  char type[16];
  char response[192];
  CanTxSdoRequest request;
  CanBridgeSubmitResult result;
  CanBridgeResult retained;
  uint32_t request_id;
  uint32_t sequence = 0U;
  int length;

  if (!JsonReadString(json, "type", type, sizeof(type)))
    return BridgeSendText(connection,
      "{\"type\":\"error\",\"error\":\"message_type_required\"}");
  if (strcmp(type, "ping") == 0)
  {
    CanBridge_Heartbeat(session_id);
    return BridgeSendText(connection, "{\"type\":\"pong\"}");
  }
  if ((strcmp(type, "sdo") != 0) ||
      !BridgeParseSdo(json, &request_id, &request))
    return BridgeSendText(connection,
      "{\"type\":\"error\",\"error\":\"invalid_sdo_request\"}");

  CanBridge_Heartbeat(session_id);
  result = CanBridge_SubmitSdo(session_id, request_id, &request, &sequence);
  length = snprintf(response, sizeof(response),
    "{\"type\":\"ack\",\"request_id\":%lu,\"tx_sequence\":%lu,"
    "\"status\":\"%s\"}", (unsigned long)request_id,
    (unsigned long)sequence, BridgeSubmitName(result));
  if ((length <= 0) || ((size_t)length >= sizeof(response)) ||
      !WebSocketSendFrame(connection, 0x01U, (const uint8_t *)response,
                          (size_t)length)) return false;
  if (result == CAN_BRIDGE_SUBMIT_DUPLICATE)
  {
    if (CanBridge_FindResult(request_id, &retained))
      return BridgeSendResult(connection, &retained);
  }
  return true;
}

static void BridgeWebSocketRun(struct netconn *connection, uint32_t session_id)
{
  CanFrameRecord frames[WEBSOCKET_BATCH_MAX_FRAMES];
  uint8_t transmit[WEBSOCKET_TX_MAX_BYTES];
  uint8_t incoming[BRIDGE_WS_TEXT_BYTES + 1U];
  WebSocketReader reader = {{0}, 0U};
  CanBridgeResult result;
  CanBridgeStatus status;
  char hello[192];
  uint32_t sent_generation = 0U;
  uint32_t lost = 0U;
  uint32_t lost_results = 0U;
  uint32_t last_ping = HAL_GetTick();
  int hello_length;

  /* Bridge sends an acknowledgement and, for a fast CANopen device, the SDO
     result in two consecutive WebSocket messages. With Nagle enabled the
     result waits for the host's delayed ACK (typically 40-50 ms on Windows),
     which caps a request/response client at about 20 SDO/s. Change the PCB
     only through the TCP/IP core so the netconn remains thread-safe. */
  if (!EnableTcpNoDelay(connection)) return;

  CanBridge_GetStatus(&status);
  hello_length = snprintf(hello, sizeof(hello),
    "{\"type\":\"hello\",\"protocol\":1,\"mode\":\"bridge\","
    "\"session_id\":%lu,\"lease_ms\":%u,\"sequence\":%lu}",
    (unsigned long)session_id, (unsigned int)CAN_BRIDGE_LEASE_MS,
    (unsigned long)bridge_stream_sequence);
  if ((hello_length <= 0) || ((size_t)hello_length >= sizeof(hello)) ||
      !WebSocketSendFrame(connection, 0x01U, (const uint8_t *)hello,
                          (size_t)hello_length)) return;

  netconn_set_recvtimeout(connection, 1);
  netconn_set_sendtimeout(connection, 1000);
  for (;;)
  {
    uint8_t opcode;
    size_t incoming_length;
    int received;
    uint32_t count;
    size_t payload_length;

    CanBridge_Update();
    CanBridge_GetStatus(&status);
    if (!status.active || !status.connected ||
        (status.session_id != session_id)) break;

    uint32_t delivered;
    for (delivered = 0U; delivered < 8U; ++delivered)
    {
      if (!CanBridge_GetNextResult(sent_generation, &result, &lost_results))
        break;
      if (!BridgeSendResult(connection, &result)) return;
      sent_generation = result.generation;
    }

    received = WebSocketReadFrame(connection, &reader, &opcode, incoming,
                                  BRIDGE_WS_TEXT_BYTES, &incoming_length);
    if (received < 0) break;
    if (received > 0)
    {
      if (opcode == 0x08U)
      {
        (void)WebSocketSendFrame(connection, 0x08U, incoming, incoming_length);
        break;
      }
      if (opcode == 0x09U)
      {
        CanBridge_Heartbeat(session_id);
        if (!WebSocketSendFrame(connection, 0x0AU, incoming, incoming_length)) break;
      }
      else if (opcode == 0x0AU)
      {
        CanBridge_Heartbeat(session_id);
      }
      else if (opcode == 0x01U)
      {
        incoming[incoming_length] = '\0';
        if (!BridgeHandleText(connection, session_id, (char *)incoming)) break;
      }
      else if (opcode != 0x02U)
      {
        break;
      }
    }

    count = WebSocketCopyBatch(&bridge_stream_sequence, &lost, frames, NULL);
    if (count != 0U)
    {
      payload_length = WebSocketBuildBatch(transmit + 4U, frames, count,
                                            bridge_stream_sequence, lost);
      transmit[0] = 0x82U;
      if (payload_length <= 125U)
      {
        memmove(transmit + 2U, transmit + 4U, payload_length);
        transmit[1] = (uint8_t)payload_length;
        payload_length += 2U;
      }
      else
      {
        transmit[1] = 126U;
        transmit[2] = (uint8_t)(payload_length >> 8U);
        transmit[3] = (uint8_t)payload_length;
        payload_length += 4U;
      }
      if (!WriteRaw(connection, transmit, payload_length)) break;
      bridge_stream_sequence += count;
      lost = 0U;
    }
    if ((uint32_t)(HAL_GetTick() - last_ping) >= BRIDGE_PING_INTERVAL_MS)
    {
      uint32_t now = HAL_GetTick();
      uint8_t ping[4];
      StoreLe32(ping, now);
      if (!WebSocketSendFrame(connection, 0x09U, ping, sizeof(ping))) break;
      last_ping = now;
    }

    /* A short cooperative delay avoids coupling the Bridge connection to the
       listener's netconn event callback. Each connection remains owned by one
       task, while SDO replies are still forwarded without the former 50 ms
       inter-packet delay. */
    if ((delivered < 8U) && (count < WEBSOCKET_BATCH_MAX_FRAMES))
      vTaskDelay(pdMS_TO_TICKS(1U));
  }
}

static void BridgeWebSocketTask(void *argument)
{
  (void)argument;
  for (;;)
  {
    struct netconn *connection;
    uint32_t session_id = 0U;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    taskENTER_CRITICAL();
    connection = bridge_pending_connection;
    session_id = bridge_pending_session_id;
    bridge_pending_connection = NULL;
    taskEXIT_CRITICAL();
    if (connection != NULL)
    {
      BridgeWebSocketRun(connection, session_id);
      (void)netconn_close(connection);
      netconn_delete(connection);
      CanBridge_Detach(session_id);
    }
    taskENTER_CRITICAL();
    bridge_websocket_reserved = false;
    taskEXIT_CRITICAL();
  }
}

static bool HandleBridgeWebSocketUpgrade(struct netconn *connection,
                                         const char *request,
                                         const char *query)
{
  uint32_t session_id = QueryUnsigned(query, "session_id", 0U, 0xFFFFFFFFUL);
  bool available;
  taskENTER_CRITICAL();
  available = !bridge_websocket_reserved;
  if (available) bridge_websocket_reserved = true;
  taskEXIT_CRITICAL();
  if (!available || !CanBridge_Attach(session_id))
  {
    taskENTER_CRITICAL();
    if (available) bridge_websocket_reserved = false;
    taskEXIT_CRITICAL();
    SendError(connection, "409 Conflict", "bridge_unavailable",
              "Bridge session is invalid, disabled, or already connected");
    return false;
  }
  if (!WebSocketAccept(connection, request))
  {
    CanBridge_Detach(session_id);
    taskENTER_CRITICAL();
    bridge_websocket_reserved = false;
    taskEXIT_CRITICAL();
    return false;
  }
  taskENTER_CRITICAL();
  bridge_pending_connection = connection;
  bridge_pending_session_id = session_id;
  taskEXIT_CRITICAL();
  xTaskNotifyGive(bridge_task_handle);
  return true;
}

static uint32_t QueryUnsigned(const char *query, const char *name,
                              uint32_t default_value, uint32_t maximum)
{
  size_t name_length = strlen(name);
  const char *cursor = query;

  if (query == NULL)
  {
    return default_value;
  }
  while (*cursor != '\0')
  {
    if ((strncmp(cursor, name, name_length) == 0) &&
        (cursor[name_length] == '='))
    {
      unsigned long value = strtoul(cursor + name_length + 1U, NULL, 10);
      return value > maximum ? maximum : (uint32_t)value;
    }
    cursor = strchr(cursor, '&');
    if (cursor == NULL)
    {
      break;
    }
    ++cursor;
  }
  return default_value;
}

static const char *FindJsonValue(const char *json, const char *key)
{
  char pattern[48];
  const char *found;
  int length = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

  if ((length <= 0) || ((size_t)length >= sizeof(pattern)))
  {
    return NULL;
  }
  found = strstr(json, pattern);
  if (found == NULL)
  {
    return NULL;
  }
  found = strchr(found + length, ':');
  if (found == NULL)
  {
    return NULL;
  }
  do
  {
    ++found;
  } while ((*found == ' ') || (*found == '\t') || (*found == '\r') || (*found == '\n'));
  return found;
}

static bool IsJsonValueDelimiter(char value)
{
  return (value == '\0') || (value == ',') || (value == '}') ||
         (value == ']') || (value == ' ') || (value == '\t') ||
         (value == '\r') || (value == '\n');
}

static bool JsonReadBool(const char *json, const char *key, bool *value)
{
  const char *found = FindJsonValue(json, key);
  if (found == NULL)
  {
    return false;
  }
  if ((strncmp(found, "true", 4U) == 0) && IsJsonValueDelimiter(found[4]))
  {
    *value = true;
    return true;
  }
  if ((strncmp(found, "false", 5U) == 0) && IsJsonValueDelimiter(found[5]))
  {
    *value = false;
    return true;
  }
  return false;
}

static bool JsonReadUnsigned(const char *json, const char *key, uint32_t *value)
{
  const char *found = FindJsonValue(json, key);
  uint32_t parsed = 0U;

  if ((found == NULL) || (*found < '0') || (*found > '9'))
  {
    return false;
  }
  do
  {
    uint32_t digit = (uint32_t)(*found - '0');
    if (parsed > ((0xFFFFFFFFUL - digit) / 10U)) return false;
    parsed = (parsed * 10U) + digit;
    ++found;
  } while ((*found >= '0') && (*found <= '9'));
  if (!IsJsonValueDelimiter(*found)) return false;
  *value = parsed;
  return true;
}

static bool JsonReadString(const char *json, const char *key,
                           char *value, size_t capacity)
{
  const char *found = FindJsonValue(json, key);
  const char *end;
  size_t length;

  if ((found == NULL) || (*found != '"') || (capacity == 0U))
  {
    return false;
  }
  ++found;
  end = strchr(found, '"');
  if (end == NULL)
  {
    return false;
  }
  length = (size_t)(end - found);
  if ((length >= capacity) || !IsJsonValueDelimiter(end[1]))
  {
    return false;
  }
  memcpy(value, found, length);
  value[length] = '\0';
  return true;
}

static bool JsonReadNodeSelection(const char *json, bool selected[128],
                                  bool *present)
{
  const char *cursor = FindJsonValue(json, "selected_nodes");
  memset(selected, 0, 128U * sizeof(selected[0]));
  *present = cursor != NULL;
  if (cursor == NULL)
  {
    return true;
  }
  if (*cursor++ != '[')
  {
    return false;
  }
  for (;;)
  {
    uint32_t value = 0U;
    uint32_t digits = 0U;
    while ((*cursor == ' ') || (*cursor == '\t') ||
           (*cursor == '\r') || (*cursor == '\n')) ++cursor;
    if (*cursor == ']')
    {
      return true;
    }
    while ((*cursor >= '0') && (*cursor <= '9'))
    {
      value = (value * 10U) + (uint32_t)(*cursor - '0');
      ++cursor;
      ++digits;
    }
    if ((digits == 0U) || (value == 0U) ||
        (value > CANOPEN_NODE_COUNT_MAX))
    {
      return false;
    }
    selected[value] = true;
    while ((*cursor == ' ') || (*cursor == '\t') ||
           (*cursor == '\r') || (*cursor == '\n')) ++cursor;
    if (*cursor == ']')
    {
      return true;
    }
    if (*cursor++ != ',')
    {
      return false;
    }
  }
}

static const char *LoggerModeName(CanLogMode mode)
{
  switch (mode)
  {
    case CAN_LOG_MODE_FULL: return "full";
    case CAN_LOG_MODE_SELECTED: return "selected";
    default: return "off";
  }
}

static bool ParseLoggerMode(const char *text, CanLogMode *mode)
{
  if (strcmp(text, "off") == 0) *mode = CAN_LOG_MODE_OFF;
  else if (strcmp(text, "full") == 0) *mode = CAN_LOG_MODE_FULL;
  else if (strcmp(text, "selected") == 0) *mode = CAN_LOG_MODE_SELECTED;
  else return false;
  return true;
}

static const char *SyncStateName(CanSyncState state)
{
  switch (state)
  {
    case CAN_SYNC_LOCKED: return "locked";
    case CAN_SYNC_MANUAL: return "manual";
    default: return "searching";
  }
}

static void WriteTxFrame(JsonWriter *writer, const CanTxFrame *frame)
{
  CanTxSdoRequest request;
  CanTxNmtCommand nmt_command;
  uint8_t nmt_node_id;
  uint32_t index;
  if (CanTx_DecodeNmt(frame, &nmt_command, &nmt_node_id))
  {
    WriterPrintf(writer,
                 "\"type\":\"nmt\",\"node_id\":%u,\"command\":\"%s\","
                 "\"can_id\":0,\"raw\":[%u,%u],\"timeout_ms\":%u",
                 (unsigned int)nmt_node_id,
                 CanTx_NmtCommandName(nmt_command),
                 (unsigned int)frame->data[0], (unsigned int)frame->data[1],
                 (unsigned int)frame->timeout_ms);
    return;
  }
  if (!CanTx_DecodeSdoRequest(frame, &request))
  {
    WriterPrintf(writer, "\"command\":\"invalid\",\"raw\":[");
    for (index = 0U; index < frame->dlc; ++index)
    {
      WriterPrintf(writer, "%s%u", index == 0U ? "" : ",",
                   (unsigned int)frame->data[index]);
    }
    WriterPrintf(writer, "]");
    return;
  }
  WriterPrintf(writer,
               "\"node_id\":%u,\"command\":\"%s\",\"index\":%u,"
               "\"subindex\":%u,\"value\":%lu,\"can_id\":%lu,\"raw\":[",
               (unsigned int)request.node_id,
               CanTx_SdoCommandName(request.command),
               (unsigned int)request.index, (unsigned int)request.subindex,
               (unsigned long)request.value, (unsigned long)frame->id);
  for (index = 0U; index < frame->dlc; ++index)
  {
    WriterPrintf(writer, "%s%u", index == 0U ? "" : ",",
                 (unsigned int)frame->data[index]);
  }
  WriterPrintf(writer, "],\"timeout_ms\":%u", (unsigned int)request.timeout_ms);
}

static void WriteTxResponse(JsonWriter *writer, const CanTxHistoryEntry *entry)
{
  uint32_t index;
  WriterPrintf(writer, ",\"has_response\":%s",
               entry->has_response ? "true" : "false");
  if (!entry->has_response) return;
  WriterPrintf(writer,
               ",\"response\":{\"timestamp_us\":%lu,\"response_time_us\":%lu,"
               "\"can_id\":%lu,\"dlc\":%u,\"raw\":[",
               (unsigned long)entry->response.timestamp_us,
               (unsigned long)entry->response_time_us,
               (unsigned long)entry->response.id,
               (unsigned int)entry->response.dlc);
  for (index = 0U; (index < entry->response.dlc) && (index < 8U); ++index)
  {
    WriterPrintf(writer, "%s%u", index == 0U ? "" : ",",
                 (unsigned int)entry->response.data[index]);
  }
  WriterChunk(writer, "]}", 2U);
}

static const char *SdoStatusName(CanopenSdoStatus status)
{
  switch (status)
  {
    case CANOPEN_SDO_STATUS_WAITING: return "waiting";
    case CANOPEN_SDO_STATUS_COMPLETED: return "completed";
    case CANOPEN_SDO_STATUS_ABORTED: return "aborted";
    case CANOPEN_SDO_STATUS_TIMEOUT: return "timeout";
    case CANOPEN_SDO_STATUS_AMBIGUOUS: return "ambiguous";
    case CANOPEN_SDO_STATUS_PROTOCOL_ERROR: return "protocol_error";
    case CANOPEN_SDO_STATUS_ORPHAN: return "orphan";
    default: return "none";
  }
}

static const char *NmtStateName(uint8_t state)
{
  switch (state)
  {
    case 0U: return "bootup";
    case 4U: return "stopped";
    case 5U: return "operational";
    case 127U: return "pre_operational";
    default: return "unknown";
  }
}

static void WriteIpv4(JsonWriter *writer, const Ipv4Address *address)
{
  WriterPrintf(writer, "\"%u.%u.%u.%u\"",
               (unsigned int)address->octet[0], (unsigned int)address->octet[1],
               (unsigned int)address->octet[2], (unsigned int)address->octet[3]);
}

static void HandleRoot(struct netconn *connection)
{
  JsonWriter writer;
  WriterBegin(&writer, connection, "200 OK");
  WriterChunk(&writer,
    "{\"api\":\"v1\",\"endpoints\":[\"/api/v1/state\","
    "\"/api/v1/config/network\",\"/api/v1/config/can\","
    "\"/api/v1/config/logger\","
    "\"/api/v1/can/mode\",\"/api/v1/can/nmt\",\"/api/v1/can/tx\","
    "\"/api/v1/can/tx/history\",\"/api/v1/can/sdo/history\","
    "\"/api/v1/can/tx/periodic\","
    "\"/api/v1/bridge/enable\",\"/api/v1/bridge/disable\","
    "\"/api/v1/bridge/status\",\"/api/v1/bridge\","
    "\"/api/v1/logger/start\",\"/api/v1/logger/stop\","
    "\"/api/v1/nodes\",\"/api/v1/statistics\","
    "\"/api/v1/statistics/reset\","
    "\"/api/v1/snapshots/frames\","
    "\"/api/v1/snapshots/nodes/{id}/events\","
    "\"/api/v1/snapshots/nodes/{id}/objects\",\"/api/v1/logs\","
    "\"/api/v1/logs/{session}/{file}\","
    "\"/api/v1/stream\"]}\n",
    strlen("{\"api\":\"v1\",\"endpoints\":[\"/api/v1/state\","
           "\"/api/v1/config/network\",\"/api/v1/config/can\","
           "\"/api/v1/config/logger\","
           "\"/api/v1/can/mode\",\"/api/v1/can/nmt\",\"/api/v1/can/tx\","
           "\"/api/v1/can/tx/history\",\"/api/v1/can/sdo/history\","
           "\"/api/v1/can/tx/periodic\","
           "\"/api/v1/bridge/enable\",\"/api/v1/bridge/disable\","
           "\"/api/v1/bridge/status\",\"/api/v1/bridge\","
           "\"/api/v1/logger/start\",\"/api/v1/logger/stop\","
           "\"/api/v1/nodes\",\"/api/v1/statistics\","
           "\"/api/v1/statistics/reset\","
           "\"/api/v1/snapshots/frames\","
           "\"/api/v1/snapshots/nodes/{id}/events\","
           "\"/api/v1/snapshots/nodes/{id}/objects\",\"/api/v1/logs\","
           "\"/api/v1/logs/{session}/{file}\","
           "\"/api/v1/stream\"]}\n"));
  WriterEnd(&writer);
}

static void HandleState(struct netconn *connection)
{
  JsonWriter writer;
  NetworkStatus network;
  CanCaptureStatus capture;
  CanLogStatus logger;
  DeviceSettings settings;

  NetworkService_GetStatus(&network);
  CanControl_GetStatus(&capture);
  CanLogger_GetStatus(&logger);
  AppSettings_Get(&settings);

  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer,
               "{\"firmware\":\"%s\",\"uptime_ms\":%lu,"
               "\"settings\":{\"schema\":%lu,\"generation\":%lu,\"storage_state\":%u},"
               "\"network\":{\"initialized\":%s,\"link_up\":%s,\"source\":\"%s\",\"ip\":",
               CAN_SNIFFER_VERSION, (unsigned long)HAL_GetTick(),
               (unsigned long)settings.schema_version,
               (unsigned long)AppSettings_GetGeneration(),
               (unsigned int)AppSettings_GetStorageState(),
               network.initialized ? "true" : "false",
               network.link_up ? "true" : "false",
               NetworkService_AddressSourceName(network.address_source));
  WriteIpv4(&writer, &network.ip);
  WriterPrintf(&writer,
               "},\"can\":{\"state\":\"%s\",\"bitrate_kbps\":%lu,\"received\":%lu,"
               "\"operating_mode\":\"%s\",\"tx_gate_enabled\":%s,"
               "\"safety_reason\":\"%s\",\"bridge_active\":%s},"
               "\"storage\":{\"state\":\"%s\",\"mode\":\"%s\",\"file\":\"%s\"}}\n",
               SyncStateName(capture.state), (unsigned long)capture.bitrate_kbps,
               (unsigned long)capture.received,
               CanControl_OperatingModeName(capture.operating_mode),
               capture.tx_gate_enabled ? "true" : "false",
               CanControl_SafetyReasonName(capture.safety_reason),
               CanBridge_IsActive() ? "true" : "false",
               CanLogger_CardStateName(logger.card_state),
               CanLogger_ModeName(logger.mode), logger.file_name);
  WriterEnd(&writer);
}

static void WriteNetworkConfig(struct netconn *connection, const char *status)
{
  JsonWriter writer;
  DeviceSettings settings;
  AppSettings_Get(&settings);
  WriterBegin(&writer, connection, status);
  WriterPrintf(&writer, "{\"dhcp_enabled\":%s,\"static_ip\":",
               settings.network.dhcp_enabled ? "true" : "false");
  WriteIpv4(&writer, &settings.network.static_ip);
  WriterChunk(&writer, ",\"netmask\":", 11U);
  WriteIpv4(&writer, &settings.network.netmask);
  WriterChunk(&writer, ",\"gateway\":", 11U);
  WriteIpv4(&writer, &settings.network.gateway);
  WriterPrintf(&writer, ",\"http_port\":%u,\"dhcp_timeout_ms\":%lu}\n",
               (unsigned int)settings.network.http_port,
               (unsigned long)settings.network.dhcp_timeout_ms);
  WriterEnd(&writer);
}

static void HandleNetworkPut(struct netconn *connection, const char *body)
{
  DeviceSettings settings;
  char text[20];
  uint32_t value;

  AppSettings_Get(&settings);
  (void)JsonReadBool(body, "dhcp_enabled", &settings.network.dhcp_enabled);
  if (JsonReadString(body, "static_ip", text, sizeof(text)) &&
      !DeviceSettings_ParseIpv4(text, &settings.network.static_ip))
  {
    SendError(connection, "400 Bad Request", "invalid_static_ip", "Invalid static IPv4 address");
    return;
  }
  if (JsonReadString(body, "netmask", text, sizeof(text)) &&
      !DeviceSettings_ParseIpv4(text, &settings.network.netmask))
  {
    SendError(connection, "400 Bad Request", "invalid_netmask", "Invalid IPv4 netmask");
    return;
  }
  if (JsonReadString(body, "gateway", text, sizeof(text)) &&
      !DeviceSettings_ParseIpv4(text, &settings.network.gateway))
  {
    SendError(connection, "400 Bad Request", "invalid_gateway", "Invalid IPv4 gateway");
    return;
  }
  if (JsonReadUnsigned(body, "http_port", &value))
  {
    if (value > 65535U)
    {
      SendError(connection, "422 Unprocessable Entity", "invalid_http_port",
                "HTTP port must be between 1 and 65535");
      return;
    }
    settings.network.http_port = (uint16_t)value;
  }
  if (JsonReadUnsigned(body, "dhcp_timeout_ms", &value))
  {
    settings.network.dhcp_timeout_ms = value;
  }
  if (!DeviceSettings_IsValidNetwork(&settings.network))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_network_config",
              "Address, netmask, gateway, port or DHCP timeout is not usable");
    return;
  }
  if (!AppSettings_Update(&settings, true))
  {
    SendError(connection, "503 Service Unavailable", "settings_write_failed",
              "QSPI settings write failed");
    return;
  }
  NetworkService_RequestApply();
  WriteNetworkConfig(connection, "202 Accepted");
}

static void WriteCanConfig(struct netconn *connection, const char *status)
{
  JsonWriter writer;
  DeviceSettings settings;
  AppSettings_Get(&settings);
  WriterBegin(&writer, connection, status);
  WriterPrintf(&writer, "{\"bitrate_kbps\":%lu,\"auto_bitrate\":%s,\"deep_sdo_enabled\":%s}\n",
               (unsigned long)settings.can_bitrate_kbps,
               settings.can_bitrate_kbps == 0U ? "true" : "false",
               settings.deep_sdo_enabled ? "true" : "false");
  WriterEnd(&writer);
}

static void HandleCanPut(struct netconn *connection, const char *body)
{
  DeviceSettings settings;
  uint32_t bitrate;

  if (CanBridge_IsActive())
  {
    SendError(connection, "409 Conflict", "bridge_active",
              "CAN settings cannot change while bridge mode is active");
    return;
  }

  AppSettings_Get(&settings);
  if (JsonReadUnsigned(body, "bitrate_kbps", &bitrate))
  {
    settings.can_bitrate_kbps = bitrate;
  }
  (void)JsonReadBool(body, "deep_sdo_enabled", &settings.deep_sdo_enabled);
  if (!DeviceSettings_IsValid(&settings))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_can_config",
              "Unsupported Classic CAN bitrate");
    return;
  }
  if (!AppSettings_Update(&settings, true))
  {
    SendError(connection, "503 Service Unavailable", "settings_write_failed",
              "QSPI settings write failed");
    return;
  }
  CanControl_RequestBitrate(settings.can_bitrate_kbps);
  WriteCanConfig(connection, "202 Accepted");
}

static void WriteBridgeStatus(struct netconn *connection, const char *http_status)
{
  JsonWriter writer;
  CanBridgeStatus status;
  CanBridgeResult result;
  CanBridge_GetStatus(&status);
  CanBridge_GetResult(&result);
  WriterBegin(&writer, connection, http_status);
  WriterPrintf(&writer,
    "{\"active\":%s,\"connected\":%s,\"session_id\":%lu,"
    "\"lease_remaining_ms\":%lu,\"request_pending\":%s,"
    "\"pending_count\":%lu,"
    "\"client_request_id\":%lu,\"tx_sequence\":%lu,"
    "\"requests\":%lu,\"completed\":%lu,\"aborted\":%lu,"
    "\"timeouts\":%lu,\"rejected\":%lu,\"unsolicited_frames\":%lu,"
    "\"last_result\":\"%s\",\"result_generation\":%lu}\n",
    status.active ? "true" : "false",
    status.connected ? "true" : "false",
    (unsigned long)status.session_id,
    (unsigned long)status.lease_remaining_ms,
    status.request_pending ? "true" : "false",
    (unsigned long)status.pending_count,
    (unsigned long)status.client_request_id,
    (unsigned long)status.tx_sequence,
    (unsigned long)status.requests,
    (unsigned long)status.completed,
    (unsigned long)status.aborted,
    (unsigned long)status.timeouts,
    (unsigned long)status.rejected,
    (unsigned long)status.unsolicited_frames,
    CanBridge_ResultName(result.code),
    (unsigned long)result.generation);
  WriterEnd(&writer);
}

static void HandleBridgeEnable(struct netconn *connection)
{
  CanBridgeStatus before;
  CanBridge_GetStatus(&before);
  if (!CanBridge_Enable(NULL))
  {
    SendError(connection, "409 Conflict", "bridge_can_busy",
              "CAN must be synchronized and the normal transmit queue must be idle");
    return;
  }
  if (!before.active)
  {
    taskENTER_CRITICAL();
    bridge_stream_sequence = websocket_write_sequence;
    taskEXIT_CRITICAL();
  }
  WriteBridgeStatus(connection, "202 Accepted");
}

static void HandleBridgeDisable(struct netconn *connection)
{
  CanBridge_Disable();
  WriteBridgeStatus(connection, "202 Accepted");
}

static void HandleCanModeGet(struct netconn *connection, const char *status)
{
  JsonWriter writer;
  CanCaptureStatus capture;
  CanTxStatus tx;
  CanControl_GetStatus(&capture);
  CanTx_GetStatus(&tx);
  WriterBegin(&writer, connection, status);
  WriterPrintf(&writer,
               "{\"mode\":\"%s\",\"tx_gate_enabled\":%s,\"active_latched\":%s,"
               "\"bitrate_kbps\":%lu,\"sync_state\":\"%s\","
               "\"safety_reason\":\"%s\",\"queue_depth\":%lu,"
               "\"transmitting\":%s,\"bridge_active\":%s}\n",
               CanControl_OperatingModeName(capture.operating_mode),
               capture.tx_gate_enabled ? "true" : "false",
               capture.active_latched ? "true" : "false",
               (unsigned long)capture.bitrate_kbps, SyncStateName(capture.state),
               CanControl_SafetyReasonName(capture.safety_reason),
               (unsigned long)tx.queue_depth, tx.transmitting ? "true" : "false",
               CanBridge_IsActive() ? "true" : "false");
  WriterEnd(&writer);
}

static void HandleCanModePost(struct netconn *connection, const char *body)
{
  char mode[16];
  bool confirmed = false;
  if (CanBridge_IsActive())
  {
    SendError(connection, "409 Conflict", "bridge_active",
              "CAN mode is controlled by the active bridge session");
    return;
  }
  if (!JsonReadString(body, "mode", mode, sizeof(mode)))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_mode",
              "mode must be active or silent");
    return;
  }
  if (strcmp(mode, "silent") == 0)
  {
    CanControl_RequestSilent();
    HandleCanModeGet(connection, "202 Accepted");
    return;
  }
  if (strcmp(mode, "active") != 0)
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_mode",
              "mode must be active or silent");
    return;
  }
  if (!JsonReadBool(body, "confirmed", &confirmed) || !confirmed)
  {
    SendError(connection, "422 Unprocessable Entity", "confirmation_required",
              "Active CAN mode requires confirmed=true");
    return;
  }
  if (!CanControl_RequestActive(true))
  {
    SendError(connection, "409 Conflict", "can_not_synchronized",
              "Select or detect a valid bitrate before enabling active mode");
    return;
  }
  HandleCanModeGet(connection, "202 Accepted");
}

static void HandleCanNmtPost(struct netconn *connection, const char *body)
{
  JsonWriter writer;
  CanTxNmtCommand command;
  CanTxSubmitResult result;
  uint32_t node_id;
  uint32_t sequence;
  char command_text[24];
  bool confirmed = false;

  if (CanBridge_IsActive())
  {
    SendError(connection, "409 Conflict", "bridge_active",
              "NMT transmission is disabled in bridge mode");
    return;
  }
  if (!JsonReadUnsigned(body, "node_id", &node_id) || (node_id > 127U))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_node_id",
              "node_id must be between 0 (broadcast) and 127");
    return;
  }
  if (!JsonReadString(body, "command", command_text, sizeof(command_text)))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_nmt_command",
              "command is required");
    return;
  }
  if (strcmp(command_text, "start") == 0) command = CAN_TX_NMT_START;
  else if (strcmp(command_text, "stop") == 0) command = CAN_TX_NMT_STOP;
  else if (strcmp(command_text, "pre_operational") == 0)
    command = CAN_TX_NMT_PRE_OPERATIONAL;
  else if (strcmp(command_text, "reset_node") == 0)
    command = CAN_TX_NMT_RESET_NODE;
  else if (strcmp(command_text, "reset_communication") == 0)
    command = CAN_TX_NMT_RESET_COMMUNICATION;
  else
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_nmt_command",
              "command must be start, stop, pre_operational, reset_node or reset_communication");
    return;
  }
  if (!JsonReadBool(body, "confirmed", &confirmed) || !confirmed)
  {
    SendError(connection, "422 Unprocessable Entity", "confirmation_required",
              "NMT transmission requires confirmed=true");
    return;
  }
  if (!CanControl_RequestActive(true))
  {
    SendError(connection, "409 Conflict", "can_not_synchronized",
              "Select or detect a valid bitrate before transmitting");
    return;
  }
  result = CanTx_SubmitNmt(command, (uint8_t)node_id, &sequence);
  if (result == CAN_TX_SUBMIT_MODE_DISABLED)
  {
    SendError(connection, "409 Conflict", "can_not_ready",
              "CAN transmit path is not available");
    return;
  }
  if (result == CAN_TX_SUBMIT_QUEUE_FULL)
  {
    SendError(connection, "429 Too Many Requests", "tx_queue_full",
              "The bounded CAN transmit queue is full");
    return;
  }
  if (result != CAN_TX_SUBMIT_OK)
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_nmt_request",
              "Invalid NMT command or node_id");
    return;
  }
  WriterBegin(&writer, connection, "202 Accepted");
  WriterPrintf(&writer,
               "{\"sequence\":%lu,\"status\":\"queued\",\"command\":\"%s\","
               "\"node_id\":%lu}\n",
               (unsigned long)sequence, CanTx_NmtCommandName(command),
               (unsigned long)node_id);
  WriterEnd(&writer);
}

static void HandleCanTxPost(struct netconn *connection, const char *body)
{
  JsonWriter writer;
  CanTxSdoRequest request = {0};
  CanTxSubmitResult result;
  bool history_saved;
  uint32_t value;
  uint32_t sequence;
  uint32_t period_ms = 0U;
  char command[16];

  if (CanBridge_IsActive())
  {
    SendError(connection, "409 Conflict", "bridge_active",
              "Local and web SDO transmission is disabled in bridge mode");
    return;
  }

  request.timeout_ms = CAN_TX_DEFAULT_TIMEOUT_MS;
  if (!JsonReadUnsigned(body, "node_id", &value) ||
      (value < 1U) || (value > 127U))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_node_id",
              "node_id must be between 1 and 127");
    return;
  }
  request.node_id = (uint8_t)value;
  if (!JsonReadUnsigned(body, "index", &value) || (value > 0xFFFFU))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_index",
              "index must be between 0 and 65535");
    return;
  }
  request.index = (uint16_t)value;
  if (!JsonReadUnsigned(body, "subindex", &value) || (value > 0xFFU))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_subindex",
              "subindex must be between 0 and 255");
    return;
  }
  request.subindex = (uint8_t)value;
  if (!JsonReadString(body, "command", command, sizeof(command)))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_sdo_command",
              "command is required");
    return;
  }
  if (strcmp(command, "read") == 0) request.command = CAN_TX_SDO_READ;
  else if (strcmp(command, "write_u8") == 0) request.command = CAN_TX_SDO_WRITE_U8;
  else if (strcmp(command, "write_u16") == 0) request.command = CAN_TX_SDO_WRITE_U16;
  else if (strcmp(command, "write_u24") == 0) request.command = CAN_TX_SDO_WRITE_U24;
  else if (strcmp(command, "write_u32") == 0) request.command = CAN_TX_SDO_WRITE_U32;
  else
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_sdo_command",
              "command must be read or write_u8/u16/u24/u32");
    return;
  }
  if (request.command != CAN_TX_SDO_READ)
  {
    if (!JsonReadUnsigned(body, "value", &request.value))
    {
      SendError(connection, "422 Unprocessable Entity", "invalid_sdo_value",
                "write commands require an unsigned value");
      return;
    }
  }
  if (JsonReadUnsigned(body, "timeout_ms", &value))
  {
    if ((value < 10U) || (value > 1000U))
    {
      SendError(connection, "422 Unprocessable Entity", "invalid_timeout",
                "timeout_ms must be between 10 and 1000");
      return;
    }
    request.timeout_ms = (uint16_t)value;
  }
  if (JsonReadUnsigned(body, "period_ms", &period_ms) &&
      ((period_ms < CAN_TX_PERIOD_MIN_MS) ||
       (period_ms > CAN_TX_PERIOD_MAX_MS) ||
       (period_ms < request.timeout_ms)))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_period",
              "period_ms must be 50..3600000 and not shorter than timeout_ms");
    return;
  }
  result = period_ms != 0U ?
    CanTx_StartPeriodicSdo(&request, period_ms, &sequence) :
    CanTx_SubmitSdo(&request, &sequence);
  if (result == CAN_TX_SUBMIT_INVALID)
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_sdo_request",
              "SDO value does not fit the selected write size");
    return;
  }
  if (result == CAN_TX_SUBMIT_MODE_DISABLED)
  {
    SendError(connection, "409 Conflict", "can_not_synchronized",
              "Select or detect a valid bitrate before transmitting");
    return;
  }
  if (result == CAN_TX_SUBMIT_QUEUE_FULL)
  {
    SendError(connection, "429 Too Many Requests", "tx_queue_full",
              period_ms != 0U ? "Periodic transmission is already active" :
                                "The bounded CAN transmit queue is full");
    return;
  }
  history_saved = SdoHistory_Remember(&request);
  WriterBegin(&writer, connection, "202 Accepted");
  WriterPrintf(&writer,
               "{\"sequence\":%lu,\"status\":\"%s\",\"period_ms\":%lu,"
               "\"history_saved\":%s}\n",
               (unsigned long)sequence,
               period_ms != 0U ? "periodic" : "queued",
               (unsigned long)period_ms, history_saved ? "true" : "false");
  WriterEnd(&writer);
}

static void HandleCanTxStatus(struct netconn *connection)
{
  JsonWriter writer;
  CanTxStatus tx;
  CanTx_GetStatus(&tx);
  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer,
    "{\"queue_depth\":%lu,\"queue_capacity\":%u,\"transmitting\":%s,"
    "\"queued\":%lu,\"sent\":%lu,\"cancelled\":%lu,\"timeouts\":%lu,"
    "\"arbitration_lost\":%lu,\"bus_off\":%lu,\"errors\":%lu,"
    "\"last_sequence\":%lu,\"last_result\":\"%s\","
    "\"rate_limit_fps\":%u,\"periodic_active\":%s,"
    "\"periodic_sequence\":%lu,\"periodic_period_ms\":%lu,"
    "\"periodic_attempts\":%lu}\n",
    (unsigned long)tx.queue_depth, CAN_TX_QUEUE_CAPACITY,
    tx.transmitting ? "true" : "false", (unsigned long)tx.queued,
    (unsigned long)tx.sent, (unsigned long)tx.cancelled,
    (unsigned long)tx.timeouts, (unsigned long)tx.arbitration_lost,
    (unsigned long)tx.bus_off, (unsigned long)tx.errors,
    (unsigned long)tx.last_sequence, CanTx_ResultName(tx.last_result),
    (unsigned int)(CanBridge_IsActive() ? 0U :
                   (1000U / CAN_TX_MIN_INTERVAL_MS)),
    tx.periodic_active ? "true" : "false",
    (unsigned long)tx.periodic_sequence,
    (unsigned long)tx.periodic_period_ms,
    (unsigned long)tx.periodic_attempts);
  WriterEnd(&writer);
}

static void HandleCanTxHistory(struct netconn *connection)
{
  JsonWriter writer;
  size_t count = CanTx_CopyHistory(tx_history_snapshot, CAN_TX_HISTORY_CAPACITY);
  size_t index;
  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer, "{\"count\":%lu,\"items\":[", (unsigned long)count);
  for (index = 0U; index < count; ++index)
  {
    CanTxHistoryEntry *entry = &tx_history_snapshot[index];
    WriterPrintf(&writer, "%s{\"sequence\":%lu,\"timestamp_us\":%lu,"
                 "\"result\":\"%s\",\"periodic\":%s,"
                 "\"attempt_count\":%lu,", index == 0U ? "" : ",",
                 (unsigned long)entry->sequence, (unsigned long)entry->timestamp_us,
                 CanTx_ResultName(entry->result),
                 entry->periodic ? "true" : "false",
                 (unsigned long)entry->attempt_count);
    WriteTxFrame(&writer, &entry->frame);
    WriteTxResponse(&writer, entry);
    WriterChunk(&writer, "}", 1U);
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

static void HandleCanTxCancel(struct netconn *connection, uint32_t sequence)
{
  JsonWriter writer;
  if (!CanTx_Cancel(sequence))
  {
    SendError(connection, "404 Not Found", "tx_sequence_not_pending",
              "Transmission is not pending or has already completed");
    return;
  }
  WriterBegin(&writer, connection, "202 Accepted");
  WriterPrintf(&writer, "{\"sequence\":%lu,\"status\":\"cancelling\"}\n",
               (unsigned long)sequence);
  WriterEnd(&writer);
}

static void HandleSdoRequestHistory(struct netconn *connection)
{
  JsonWriter writer;
  CanTxSdoRequest requests[SDO_HISTORY_CAPACITY];
  size_t count = SdoHistory_Copy(requests, SDO_HISTORY_CAPACITY);
  size_t index;

  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer, "{\"count\":%lu,\"items\":[", (unsigned long)count);
  for (index = 0U; index < count; ++index)
  {
    const CanTxSdoRequest *request = &requests[index];
    WriterPrintf(&writer,
                 "%s{\"node_id\":%u,\"command\":\"%s\","
                 "\"index\":%u,\"subindex\":%u,\"value\":%lu,"
                 "\"timeout_ms\":%u}",
                 index == 0U ? "" : ",", (unsigned int)request->node_id,
                 CanTx_SdoCommandName(request->command),
                 (unsigned int)request->index,
                 (unsigned int)request->subindex,
                 (unsigned long)request->value,
                 (unsigned int)request->timeout_ms);
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

static void HandleCanTxPeriodicStop(struct netconn *connection)
{
  JsonWriter writer;
  bool stopped = CanTx_StopPeriodic();
  WriterBegin(&writer, connection, stopped ? "202 Accepted" : "200 OK");
  WriterPrintf(&writer, "{\"periodic_active\":false,\"stopped\":%s}\n",
               stopped ? "true" : "false");
  WriterEnd(&writer);
}

static void WriteLoggerConfig(struct netconn *connection, const char *status)
{
  JsonWriter writer;
  CanLogStatus logger;
  uint32_t node_id;
  bool first = true;
  CanLogger_GetStatus(&logger);
  if (logger.mode != CAN_LOG_MODE_OFF)
  {
    logger_configured_mode = logger.mode;
  }
  WriterBegin(&writer, connection, status);
  WriterPrintf(&writer,
    "{\"mode\":\"%s\",\"recording\":%s,\"active_mode\":\"%s\","
    "\"card_state\":\"%s\",\"file\":\"%s\","
    "\"file_size_bytes\":%llu,\"free_bytes\":%llu,"
    "\"stored_frames\":%llu,\"dropped_frames\":%llu,"
    "\"write_errors\":%lu,\"recoveries\":%lu,\"last_error\":%ld,"
    "\"selected_node_count\":%lu,\"buffered_blocks\":%lu,"
    "\"sd_io\":{\"errors\":%lu,\"retries\":%lu,"
    "\"last_error_tick\":%lu,\"last_sector\":%lu,\"last_count\":%lu,"
    "\"last_hal_error\":%lu,\"last_sd_status\":%lu,"
    "\"max_read_ms\":%lu,\"max_write_ms\":%lu,"
    "\"last_operation\":%u,\"last_hal_status\":%u,"
    "\"last_handle_state\":%u},"
    "\"selected_nodes\":[",
    LoggerModeName(logger_configured_mode),
    logger.mode != CAN_LOG_MODE_OFF ? "true" : "false",
    LoggerModeName(logger.mode), CanLogger_CardStateName(logger.card_state),
    logger.file_name, (unsigned long long)logger.file_size_bytes,
    (unsigned long long)logger.free_bytes,
    (unsigned long long)logger.stored_frames,
    (unsigned long long)logger.dropped_frames,
    (unsigned long)logger.write_errors, (unsigned long)logger.recovery_count,
    (long)logger.last_error, (unsigned long)logger.selected_node_count,
    (unsigned long)logger.buffered_blocks,
    (unsigned long)logger.sd_io_errors, (unsigned long)logger.sd_retries,
    (unsigned long)logger.sd_last_error_tick,
    (unsigned long)logger.sd_last_sector,
    (unsigned long)logger.sd_last_count,
    (unsigned long)logger.sd_last_hal_error,
    (unsigned long)logger.sd_last_status,
    (unsigned long)logger.sd_max_read_ms,
    (unsigned long)logger.sd_max_write_ms,
    (unsigned int)logger.sd_last_operation,
    (unsigned int)logger.sd_last_hal_status,
    (unsigned int)logger.sd_last_handle_state);
  for (node_id = 1U; node_id <= CANOPEN_NODE_COUNT_MAX; ++node_id)
  {
    if (CanLogger_IsNodeSelected((uint8_t)node_id))
    {
      WriterPrintf(&writer, "%s%lu", first ? "" : ",",
                   (unsigned long)node_id);
      first = false;
    }
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

static void HandleLoggerPut(struct netconn *connection, const char *body)
{
  CanLogMode mode = logger_configured_mode;
  CanLogMode active_mode = CanLogger_GetMode();
  bool selected[128];
  bool selection_present;
  char mode_text[16];
  uint32_t node_id;

  if ((FindJsonValue(body, "mode") != NULL) &&
      (!JsonReadString(body, "mode", mode_text, sizeof(mode_text)) ||
       !ParseLoggerMode(mode_text, &mode)))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_logger_mode",
              "Logger mode must be off, full or selected");
    return;
  }
  if (mode == CAN_LOG_MODE_OFF)
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_logger_mode",
              "Logger configuration mode must be full or selected; use the stop endpoint");
    return;
  }
  if (!JsonReadNodeSelection(body, selected, &selection_present))
  {
    SendError(connection, "422 Unprocessable Entity", "invalid_selected_nodes",
              "selected_nodes must contain CANopen node IDs from 1 to 127");
    return;
  }
  if (selection_present)
  {
    CanLogger_ClearSelectedNodes();
    for (node_id = 1U; node_id <= CANOPEN_NODE_COUNT_MAX; ++node_id)
    {
      if (selected[node_id])
      {
        CanLogger_SetNodeSelected((uint8_t)node_id, true);
      }
    }
  }
  logger_configured_mode = mode;
  if (active_mode != CAN_LOG_MODE_OFF)
  {
    CanLogger_SetMode(logger_configured_mode);
  }
  WriteLoggerConfig(connection, "202 Accepted");
}

static void HandleLoggerStart(struct netconn *connection)
{
  CanLogger_SetMode(logger_configured_mode);
  WriteLoggerConfig(connection, "202 Accepted");
}

static void HandleLoggerStop(struct netconn *connection)
{
  CanLogger_SetMode(CAN_LOG_MODE_OFF);
  WriteLoggerConfig(connection, "202 Accepted");
}

static void HandleNodes(struct netconn *connection, const char *query)
{
  JsonWriter writer;
  CanopenGlobalSnapshot global;
  uint32_t count = CanopenAnalyzer_GetNodesSnapshot(node_snapshot,
                                                     CANOPEN_NODE_COUNT_MAX, &global);
  uint32_t offset = QueryUnsigned(query, "offset", 0U, CANOPEN_NODE_COUNT_MAX);
  uint32_t limit = QueryUnsigned(query, "limit", API_DEFAULT_PAGE_SIZE, API_MAX_PAGE_SIZE);
  uint32_t end = offset + limit;
  uint32_t index;
  if (end > count) end = count;
  if (offset > count) offset = count;

  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer,
               "{\"total\":%lu,\"offset\":%lu,\"analyzer_frames\":%lu,"
               "\"snapshot_us\":%lu,\"items\":[",
               (unsigned long)count, (unsigned long)offset,
               (unsigned long)global.frame_count,
               (unsigned long)global.last_timestamp_us);
  for (index = offset; index < end; ++index)
  {
    const CanopenNodeSnapshot *node = &node_snapshot[index];
    WriterPrintf(&writer,
      "%s{\"node_id\":%u,\"last_type\":\"%s\",\"first_seen_us\":%lu,"
      "\"last_seen_us\":%lu,\"frames\":%lu,\"bytes\":%lu,"
      "\"sdo_requests\":%lu,\"sdo_responses\":%lu,\"sdo_started\":%lu,"
      "\"sdo_completed\":%lu,\"sdo_aborted\":%lu,\"sdo_timeouts\":%lu,"
      "\"sdo_conflicts\":%lu,\"sdo_orphans\":%lu,"
      "\"sdo_protocol_errors\":%lu,\"sdo_errors\":%lu,"
      "\"has_nmt_state\":%s,\"nmt_state\":\"%s\",\"nmt_state_code\":%u}",
      index == offset ? "" : ",", (unsigned int)node->node_id,
      CanopenDecoder_TypeName(node->last_type),
      (unsigned long)node->first_seen_us, (unsigned long)node->last_seen_us,
      (unsigned long)node->frame_count, (unsigned long)node->byte_count,
      (unsigned long)node->sdo_request_count, (unsigned long)node->sdo_response_count,
      (unsigned long)node->sdo_started_count,
      (unsigned long)node->sdo_completed_count, (unsigned long)node->sdo_aborted_count,
      (unsigned long)node->sdo_timeout_count,
      (unsigned long)node->sdo_conflict_count,
      (unsigned long)node->sdo_orphan_count,
      (unsigned long)node->sdo_protocol_error_count,
      (unsigned long)(node->sdo_protocol_error_count + node->sdo_conflict_count +
                      node->sdo_orphan_count),
      node->has_nmt_state ? "true" : "false",
      node->has_nmt_state ? NmtStateName(node->nmt_state) : "unknown",
      (unsigned int)node->nmt_state);
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

static void HandleStatistics(struct netconn *connection)
{
  JsonWriter writer;
  CanCaptureStatus capture;
  CanopenGlobalSnapshot analyzer;
  CanLogStatus logger;
  ApiServerStatus api;
  CanTxStatus tx;

  CanControl_GetStatus(&capture);
  CanopenAnalyzer_GetGlobalSnapshot(&analyzer);
  CanLogger_GetStatus(&logger);
  ApiServer_GetStatus(&api);
  CanTx_GetStatus(&tx);
  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer,
    "{\"can\":{\"received\":%lu,\"ring_overflows\":%lu,\"gui_overflows\":%lu,"
    "\"errors\":%lu,\"bus_load_permille\":%u,"
    "\"error_counters\":{\"interrupts\":%lu,\"warning\":%lu,"
    "\"passive\":%lu,\"bus_off\":%lu,\"stuff\":%lu,\"form\":%lu,"
    "\"ack\":%lu,\"bit_recessive\":%lu,\"bit_dominant\":%lu,"
    "\"crc\":%lu,\"unclassified\":%lu},"
    "\"last_error\":{\"type\":\"%s\",\"timestamp_us\":%lu,"
    "\"esr\":%lu,\"tx_error_counter\":%u,\"rx_error_counter\":%u}},",
    (unsigned long)capture.received, (unsigned long)capture.ring_overflows,
    (unsigned long)capture.gui_overflows, (unsigned long)capture.errors,
    (unsigned int)capture.bus_load_permille,
    (unsigned long)capture.error_statistics.interrupts,
    (unsigned long)capture.error_statistics.warning,
    (unsigned long)capture.error_statistics.passive,
    (unsigned long)capture.error_statistics.bus_off,
    (unsigned long)capture.error_statistics.stuff,
    (unsigned long)capture.error_statistics.form,
    (unsigned long)capture.error_statistics.ack,
    (unsigned long)capture.error_statistics.bit_recessive,
    (unsigned long)capture.error_statistics.bit_dominant,
    (unsigned long)capture.error_statistics.crc,
    (unsigned long)capture.error_statistics.unclassified,
    CanControl_ErrorName(capture.error_statistics.last_error),
    (unsigned long)capture.error_statistics.last_timestamp_us,
    (unsigned long)capture.error_statistics.last_esr,
    (unsigned int)capture.error_statistics.last_tx_error_counter,
    (unsigned int)capture.error_statistics.last_rx_error_counter);
  WriterPrintf(&writer,
    "\"analyzer\":{\"frames\":%lu,\"unique_nodes\":%lu,\"queue_overflows\":%lu},"
    "\"logger\":{\"stored_frames\":%llu,\"dropped_frames\":%llu,"
    "\"write_errors\":%lu,\"recoveries\":%lu,\"buffered_blocks\":%lu,"
    "\"sd_io_errors\":%lu,\"sd_retries\":%lu,"
    "\"sd_max_write_ms\":%lu},"
    "\"tx\":{\"queued\":%lu,\"sent\":%lu,\"cancelled\":%lu,"
    "\"timeouts\":%lu,\"arbitration_lost\":%lu,\"bus_off\":%lu,"
    "\"errors\":%lu},"
    "\"websocket\":{\"active\":%s,\"connections\":%lu,\"batches\":%lu,"
    "\"frames\":%lu,\"lost_frames\":%lu},"
    "\"downloads\":{\"requests\":%lu,\"completed\":%lu,\"bytes\":%llu}}\n",
    (unsigned long)analyzer.frame_count,
    (unsigned long)analyzer.unique_node_count,
    (unsigned long)CanopenAnalyzer_GetQueueOverflows(),
    (unsigned long long)logger.stored_frames,
    (unsigned long long)logger.dropped_frames,
    (unsigned long)logger.write_errors, (unsigned long)logger.recovery_count,
    (unsigned long)logger.buffered_blocks,
    (unsigned long)logger.sd_io_errors, (unsigned long)logger.sd_retries,
    (unsigned long)logger.sd_max_write_ms,
    (unsigned long)tx.queued, (unsigned long)tx.sent,
    (unsigned long)tx.cancelled, (unsigned long)tx.timeouts,
    (unsigned long)tx.arbitration_lost, (unsigned long)tx.bus_off,
    (unsigned long)tx.errors,
    api.websocket_active ? "true" : "false",
    (unsigned long)api.websocket_connections,
    (unsigned long)api.websocket_batches,
    (unsigned long)api.websocket_frames,
    (unsigned long)api.websocket_lost_frames,
    (unsigned long)api.download_requests,
    (unsigned long)api.download_completed,
    (unsigned long long)api.download_bytes);
  WriterEnd(&writer);
}

static void HandleStatisticsReset(struct netconn *connection)
{
  JsonWriter writer;
  CanPipeline_ResetStatistics();
  taskENTER_CRITICAL();
  frame_history_write = 0U;
  frame_history_count = 0U;
  public_status.websocket_batches = 0U;
  public_status.websocket_frames = 0U;
  public_status.websocket_lost_frames = 0U;
  public_status.download_requests = 0U;
  public_status.download_completed = 0U;
  public_status.download_bytes = 0U;
  taskEXIT_CRITICAL();
  WriterBegin(&writer, connection, "202 Accepted");
  WriterChunk(&writer, "{\"reset\":true}\n", 15U);
  WriterEnd(&writer);
}

static uint32_t CopyFrameSnapshot(void)
{
  uint32_t count;
  uint32_t start;
  uint32_t index;

  taskENTER_CRITICAL();
  count = frame_history_count;
  start = (frame_history_write + API_FRAME_SNAPSHOT_MAX - count) % API_FRAME_SNAPSHOT_MAX;
  for (index = 0U; index < count; ++index)
  {
    frame_snapshot[index] = frame_history[(start + index) % API_FRAME_SNAPSHOT_MAX];
  }
  taskEXIT_CRITICAL();
  return count;
}

static void HandleFrames(struct netconn *connection, const char *query)
{
  JsonWriter writer;
  uint32_t count = CopyFrameSnapshot();
  uint32_t offset = QueryUnsigned(query, "offset", 0U, API_FRAME_SNAPSHOT_MAX);
  uint32_t limit = QueryUnsigned(query, "limit", API_DEFAULT_PAGE_SIZE, API_MAX_PAGE_SIZE);
  uint32_t end = offset + limit;
  uint32_t index;
  if (offset > count) offset = count;
  if (end > count) end = count;

  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer, "{\"total\":%lu,\"offset\":%lu,\"items\":[",
               (unsigned long)count, (unsigned long)offset);
  for (index = offset; index < end; ++index)
  {
    const CanFrameRecord *frame = &frame_snapshot[index];
    uint32_t byte;
    WriterPrintf(&writer,
      "%s{\"timestamp_us\":%lu,\"capture_time_us\":%llu,\"id\":%lu,"
      "\"extended\":%s,\"remote\":%s,\"local_tx\":%s,\"dlc\":%u,\"data\":\"",
      index == offset ? "" : ",", (unsigned long)frame->timestamp_us,
      (unsigned long long)(((uint64_t)frame->capture_time_high << 32) | frame->capture_time_us),
      (unsigned long)frame->id,
      (frame->flags & CAN_FRAME_FLAG_EXTENDED) != 0U ? "true" : "false",
      (frame->flags & CAN_FRAME_FLAG_REMOTE) != 0U ? "true" : "false",
      (frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U ? "true" : "false",
      (unsigned int)frame->dlc);
    for (byte = 0U; byte < frame->dlc && byte < 8U; ++byte)
    {
      WriterPrintf(&writer, "%02X", (unsigned int)frame->data[byte]);
    }
    WriterChunk(&writer, "\"}", 2U);
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

static void HandleNodeEvents(struct netconn *connection, uint8_t node_id,
                             bool objects, const char *query)
{
  JsonWriter writer;
  uint32_t count = objects ?
    CanopenAnalyzer_GetNodeObjectsSnapshot(node_id, event_snapshot, CANOPEN_EVENT_SNAPSHOT_MAX) :
    CanopenAnalyzer_GetNodeEventsSnapshot(node_id, event_snapshot, CANOPEN_EVENT_SNAPSHOT_MAX);
  uint32_t limit = QueryUnsigned(query, "limit", API_DEFAULT_PAGE_SIZE, API_MAX_PAGE_SIZE);
  uint32_t begin = objects && (count > limit) ? count - limit : 0U;
  uint32_t end = !objects && (count > limit) ? limit : count;
  uint32_t index;

  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer, "{\"node_id\":%u,\"kind\":\"%s\",\"total\":%lu,\"items\":[",
               (unsigned int)node_id, objects ? "objects" : "events", (unsigned long)count);
  for (index = begin; index < end; ++index)
  {
    const CanopenNodeEventSnapshot *event = &event_snapshot[index];
    uint32_t byte;
    WriterPrintf(&writer,
      "%s{\"timestamp_us\":%lu,\"id\":%lu,\"type\":\"%s\","
      "\"sdo_status\":\"%s\",\"index\":%u,\"subindex\":%u,"
      "\"response_time_us\":%lu,\"transaction_time_us\":%lu,"
      "\"transferred_bytes\":%lu,\"abort_code\":%lu,"
      "\"extended\":%s,\"remote\":%s,\"local_tx\":%s,"
      "\"local_transaction\":%s,\"dlc\":%u,"
      "\"has_response_time\":%s,\"has_sdo_address\":%s,"
      "\"data_offset\":%u,\"data\":\"",
      index == begin ? "" : ",", (unsigned long)event->frame.timestamp_us,
      (unsigned long)event->frame.id, CanopenDecoder_TypeName(event->type),
      SdoStatusName(event->sdo_status), (unsigned int)event->object_index,
      (unsigned int)event->object_subindex, (unsigned long)event->response_time_us,
      (unsigned long)event->transaction_time_us,
      (unsigned long)event->transferred_bytes, (unsigned long)event->abort_code,
      (event->frame.flags & CAN_FRAME_FLAG_EXTENDED) != 0U ? "true" : "false",
      (event->frame.flags & CAN_FRAME_FLAG_REMOTE) != 0U ? "true" : "false",
      (event->frame.flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U ? "true" : "false",
      event->local_transaction ? "true" : "false",
      (unsigned int)event->frame.dlc,
      event->has_response_time ? "true" : "false",
      event->has_sdo_address ? "true" : "false",
      (unsigned int)event->data_offset);
    for (byte = 0U; byte < event->frame.dlc && byte < 8U; ++byte)
    {
      WriterPrintf(&writer, "%02X", (unsigned int)event->frame.data[byte]);
    }
    WriterChunk(&writer, "\"}", 2U);
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

static void HandleLogs(struct netconn *connection, const char *query)
{
  JsonWriter writer;
  uint32_t count = 0U;
  uint32_t total = 0U;
  uint32_t limit = QueryUnsigned(query, "limit", CAN_LOG_LIST_MAX,
                                 CAN_LOG_LIST_MAX);
  uint32_t before_session_id = QueryUnsigned(query, "before_session", 0U,
                                             999999U);
  uint32_t before_file_index = QueryUnsigned(query, "before_file", 0U,
                                             999999U);
  bool has_more = false;
  uint32_t index;

  if (limit == 0U)
  {
    limit = 1U;
  }
  if ((before_session_id == 0U) || (before_file_index == 0U))
  {
    before_session_id = 0U;
    before_file_index = 0U;
  }
  if (!CanLogger_GetLogListPage(log_snapshot, limit, before_session_id,
                                before_file_index, &count, &total,
                                &has_more, 5000U))
  {
    SendError(connection, "503 Service Unavailable", "storage_unavailable",
              "Completed log list is not available");
    return;
  }
  WriterBegin(&writer, connection, "200 OK");
  WriterPrintf(&writer,
               "{\"count\":%lu,\"total\":%lu,\"limit\":%lu,"
               "\"order\":\"newest_first\",\"has_more\":%s,"
               "\"truncated\":%s,"
               "\"next_cursor\":",
               (unsigned long)count, (unsigned long)total,
               (unsigned long)limit, has_more ? "true" : "false",
               has_more ? "true" : "false");
  if (has_more && (count > 0U))
  {
    const CanLogFileInfo *last = &log_snapshot[count - 1U];
    WriterPrintf(&writer, "{\"session_id\":%lu,\"file_index\":%lu}",
                 (unsigned long)last->session_id,
                 (unsigned long)last->file_index);
  }
  else
  {
    WriterChunk(&writer, "null", 4U);
  }
  WriterChunk(&writer, ",\"items\":[", 10U);
  for (index = 0U; index < count; ++index)
  {
    static const char prefix[] = "/CANSNIFF/LOGS/";
    const char *relative = strncmp(log_snapshot[index].path, prefix,
                                   sizeof(prefix) - 1U) == 0 ?
                           log_snapshot[index].path + sizeof(prefix) - 1U : "";
    WriterPrintf(&writer,
      "%s{\"path\":\"%s\",\"session_id\":%lu,\"file_index\":%lu,"
      "\"size_bytes\":%llu,"
      "\"download_url\":\"/api/v1/logs/%s\"}",
      index == 0U ? "" : ",", log_snapshot[index].path,
      (unsigned long)log_snapshot[index].session_id,
      (unsigned long)log_snapshot[index].file_index,
      (unsigned long long)log_snapshot[index].size_bytes, relative);
  }
  WriterChunk(&writer, "]}\n", 3U);
  WriterEnd(&writer);
}

typedef struct
{
  bool present;
  bool has_end;
  uint64_t start;
  uint64_t end;
} HttpByteRange;

static bool ParseDecimal64(const char **cursor, uint64_t *value)
{
  uint64_t parsed = 0U;
  uint32_t digits = 0U;
  while ((**cursor >= '0') && (**cursor <= '9'))
  {
    uint32_t digit = (uint32_t)(**cursor - '0');
    if (parsed > ((UINT64_MAX - digit) / 10U))
    {
      return false;
    }
    parsed = (parsed * 10U) + digit;
    ++*cursor;
    ++digits;
  }
  if (digits == 0U)
  {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ParseRangeHeader(const char *request, HttpByteRange *range)
{
  char value[64];
  const char *cursor;
  memset(range, 0, sizeof(*range));
  if (!FindHeaderValue(request, "Range", value, sizeof(value)))
  {
    return true;
  }
  range->present = true;
  if (strncmp(value, "bytes=", 6U) != 0)
  {
    return false;
  }
  cursor = value + 6U;
  if (!ParseDecimal64(&cursor, &range->start) || (*cursor != '-'))
  {
    return false;
  }
  ++cursor;
  if (*cursor != '\0')
  {
    if (!ParseDecimal64(&cursor, &range->end) || (*cursor != '\0') ||
        (range->end < range->start))
    {
      return false;
    }
    range->has_end = true;
  }
  return true;
}

static bool ParseLogDownloadPath(const char *path, char storage_path[72],
                                 char filename[32])
{
  static const char prefix[] = "/api/v1/logs/";
  const char *tail;
  uint32_t index;
  if ((path == NULL) ||
      (strncmp(path, prefix, sizeof(prefix) - 1U) != 0))
  {
    return false;
  }
  tail = path + sizeof(prefix) - 1U;
  if ((strlen(tail) != 19U) || (tail[0] != 'S') || (tail[7] != '/') ||
      (tail[8] != 'L') || (strcmp(tail + 15U, ".CSL") != 0))
  {
    return false;
  }
  for (index = 1U; index <= 6U; ++index)
  {
    if ((tail[index] < '0') || (tail[index] > '9')) return false;
  }
  for (index = 9U; index <= 14U; ++index)
  {
    if ((tail[index] < '0') || (tail[index] > '9')) return false;
  }
  (void)snprintf(storage_path, 72U, "/CANSNIFF/LOGS/%s", tail);
  (void)snprintf(filename, 32U, "%.7s_%s", tail, tail + 8U);
  return true;
}

static void SendRangeNotSatisfiable(struct netconn *connection, uint64_t size)
{
  char header[192];
  int length = snprintf(header, sizeof(header),
    "HTTP/1.1 416 Range Not Satisfiable\r\n"
    "Content-Range: bytes */%llu\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n", (unsigned long long)size);
  if ((length <= 0) || ((size_t)length >= sizeof(header)) ||
      !WriteRaw(connection, header, (size_t)length))
  {
    StatusIncrement(&public_status.send_errors);
  }
}

static void HandleLogDownload(struct netconn *connection, const char *request,
                              const char *storage_path, const char *filename,
                              bool send_body)
{
  CanLogDownload download;
  CanLogDownloadResult result;
  HttpByteRange range;
  uint64_t start = 0U;
  uint64_t end;
  uint64_t remaining;
  uint64_t sent = 0U;
  char header[384];
  int header_length;
  bool success = false;

  StatusIncrement(&public_status.download_requests);
  /* Browsers and antivirus filters may briefly pause a download while they
     choose a target or inspect the response. One second is too aggressive. */
  netconn_set_sendtimeout(connection, 30000);
  if (!ParseRangeHeader(request, &range))
  {
    SendError(connection, "400 Bad Request", "bad_range",
              "Only one bytes=start-end range is supported");
    return;
  }
  result = CanLogger_DownloadBegin(&download, storage_path, 5000U);
  if (result != CAN_LOG_DOWNLOAD_OK)
  {
    if (result == CAN_LOG_DOWNLOAD_NOT_FOUND)
      SendError(connection, "404 Not Found", "log_not_found",
                "Completed log file was not found");
    else if (result == CAN_LOG_DOWNLOAD_INVALID_PATH)
      SendError(connection, "400 Bad Request", "invalid_log_path",
                "Invalid completed log path");
    else if (result == CAN_LOG_DOWNLOAD_UNAVAILABLE)
      SendError(connection, "503 Service Unavailable", "storage_unavailable",
                "microSD storage is not available");
    else
      SendError(connection, "500 Internal Server Error", "storage_error",
                "The log file could not be opened");
    return;
  }

  if (range.present)
  {
    start = range.start;
  }
  if ((download.size_bytes == 0U) || (start >= download.size_bytes))
  {
    SendRangeNotSatisfiable(connection, download.size_bytes);
    CanLogger_DownloadEnd(&download, 5000U);
    return;
  }
  end = download.size_bytes - 1U;
  if (range.present && range.has_end && (range.end < end))
  {
    end = range.end;
  }
  remaining = end - start + 1U;
  if (!CanLogger_DownloadSeek(&download, start, 5000U))
  {
    CanLogger_DownloadEnd(&download, 5000U);
    SendError(connection, "500 Internal Server Error", "storage_error",
              "The log file could not be positioned");
    return;
  }

  if (range.present)
  {
    header_length = snprintf(header, sizeof(header),
      "HTTP/1.1 206 Partial Content\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\n"
      "Accept-Ranges: bytes\r\n"
      "Content-Range: bytes %llu-%llu/%llu\r\n"
      "Content-Length: %llu\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n",
      filename, (unsigned long long)start, (unsigned long long)end,
      (unsigned long long)download.size_bytes,
      (unsigned long long)remaining);
  }
  else
  {
    header_length = snprintf(header, sizeof(header),
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\n"
      "Accept-Ranges: bytes\r\n"
      "Content-Length: %llu\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n",
      filename, (unsigned long long)remaining);
  }
  if ((header_length > 0) && ((size_t)header_length < sizeof(header)) &&
      WriteRaw(connection, header, (size_t)header_length))
  {
    success = true;
    if (!send_body)
    {
      remaining = 0U;
    }
    while (send_body && (remaining > 0U))
    {
      const uint8_t *data;
      uint32_t length;
      uint32_t requested = remaining > CAN_LOG_DOWNLOAD_BUFFER_BYTES ?
                           CAN_LOG_DOWNLOAD_BUFFER_BYTES : (uint32_t)remaining;
      if (!CanLogger_DownloadRead(&download, requested, &data, &length, 5000U) ||
          (length == 0U) || !WriteRaw(connection, data, length))
      {
        success = false;
        break;
      }
      remaining -= length;
      sent += length;
    }
  }
  else
  {
    StatusIncrement(&public_status.send_errors);
  }
  CanLogger_DownloadEnd(&download, 5000U);
  StatusAdd64(&public_status.download_bytes, sent);
  if (success && (remaining == 0U))
  {
    StatusIncrement(&public_status.download_completed);
  }
  else if (remaining != 0U)
  {
    StatusIncrement(&public_status.send_errors);
  }
}

static ApiRequestResult ReceiveRequest(struct netconn *connection, char *request,
                                       size_t capacity, size_t *received_length)
{
  size_t used = 0U;
  size_t expected = 0U;
  bool have_headers = false;

  while (used + 1U < capacity)
  {
    struct netbuf *buffer = NULL;
    u16_t length;
    err_t error = netconn_recv(connection, &buffer);
    if ((error != ERR_OK) || (buffer == NULL))
    {
      if (buffer != NULL) netbuf_delete(buffer);
      return API_REQUEST_INCOMPLETE;
    }
    length = netbuf_len(buffer);
    if ((size_t)length > capacity - used - 1U)
    {
      netbuf_delete(buffer);
      return API_REQUEST_TOO_LARGE;
    }
    (void)netbuf_copy(buffer, request + used, length);
    used += length;
    request[used] = '\0';
    netbuf_delete(buffer);

    if (!have_headers)
    {
      char *header_end = strstr(request, "\r\n\r\n");
      if (header_end != NULL)
      {
        const char *content_length = strstr(request, "Content-Length:");
        have_headers = true;
        expected = (size_t)(header_end + 4 - request);
        if ((content_length != NULL) && (content_length < header_end))
        {
          expected += (size_t)strtoul(content_length + 15, NULL, 10);
        }
      }
    }
    if (have_headers && (used >= expected))
    {
      *received_length = used;
      return API_REQUEST_OK;
    }
  }
  return API_REQUEST_TOO_LARGE;
}

static bool ParseRequestLine(const char *request, char *method, size_t method_capacity,
                             char *target, size_t target_capacity)
{
  const char *method_end;
  const char *target_start;
  const char *target_end;
  size_t method_length;
  size_t target_length;

  if ((request == NULL) || (method == NULL) || (target == NULL))
  {
    return false;
  }
  method_end = strchr(request, ' ');
  if (method_end == NULL)
  {
    return false;
  }
  target_start = method_end + 1;
  target_end = strchr(target_start, ' ');
  if ((target_end == NULL) || (strstr(target_end + 1, "\r\n") == NULL))
  {
    return false;
  }
  method_length = (size_t)(method_end - request);
  target_length = (size_t)(target_end - target_start);
  if ((method_length == 0U) || (method_length >= method_capacity) ||
      (target_length == 0U) || (target_length >= target_capacity))
  {
    return false;
  }
  memcpy(method, request, method_length);
  method[method_length] = '\0';
  memcpy(target, target_start, target_length);
  target[target_length] = '\0';
  return true;
}

static bool ParseNodePath(const char *path, const char *suffix, uint8_t *node_id)
{
  static const char prefix[] = "/api/v1/snapshots/nodes/";
  const char *cursor;
  uint32_t value = 0U;
  uint32_t digits = 0U;

  if ((path == NULL) || (suffix == NULL) || (node_id == NULL) ||
      (strncmp(path, prefix, sizeof(prefix) - 1U) != 0))
  {
    return false;
  }
  cursor = path + sizeof(prefix) - 1U;
  while ((*cursor >= '0') && (*cursor <= '9'))
  {
    value = (value * 10U) + (uint32_t)(*cursor - '0');
    ++cursor;
    ++digits;
    if (value > CANOPEN_NODE_COUNT_MAX)
    {
      return false;
    }
  }
  if ((digits == 0U) || (value == 0U) || (strcmp(cursor, suffix) != 0))
  {
    return false;
  }
  *node_id = (uint8_t)value;
  return true;
}

static bool ParseTxCancelPath(const char *path, uint32_t *sequence)
{
  static const char prefix[] = "/api/v1/can/tx/";
  static const char suffix[] = "/cancel";
  const char *cursor;
  uint32_t value = 0U;
  uint32_t digits = 0U;
  if ((path == NULL) || (sequence == NULL) ||
      (strncmp(path, prefix, sizeof(prefix) - 1U) != 0)) return false;
  cursor = path + sizeof(prefix) - 1U;
  while ((*cursor >= '0') && (*cursor <= '9'))
  {
    uint32_t digit = (uint32_t)(*cursor - '0');
    if (value > ((0xFFFFFFFFUL - digit) / 10U)) return false;
    value = (value * 10U) + digit;
    ++cursor;
    ++digits;
  }
  if ((digits == 0U) || (value == 0U) || (strcmp(cursor, suffix) != 0)) return false;
  *sequence = value;
  return true;
}

static bool Dispatch(struct netconn *connection, char *request)
{
  char method[8];
  char target[160];
  char path[160];
  char log_path[72];
  char log_filename[32];
  char *query;
  char *body = strstr(request, "\r\n\r\n");
  uint8_t node_id;
  uint32_t tx_sequence;

  if (!ParseRequestLine(request, method, sizeof(method), target, sizeof(target)) ||
      (body == NULL))
  {
    StatusIncrement(&public_status.bad_requests);
    SendError(connection, "400 Bad Request", "bad_request", "Malformed HTTP request");
    return false;
  }
  body += 4;
  (void)strncpy(path, target, sizeof(path) - 1U);
  path[sizeof(path) - 1U] = '\0';
  query = strchr(path, '?');
  if (query != NULL)
  {
    *query++ = '\0';
  }

  if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1") == 0))
    HandleRoot(connection);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/state") == 0))
    HandleState(connection);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/config/network") == 0))
    WriteNetworkConfig(connection, "200 OK");
  else if ((strcmp(method, "PUT") == 0) && (strcmp(path, "/api/v1/config/network") == 0))
    HandleNetworkPut(connection, body);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/config/can") == 0))
    WriteCanConfig(connection, "200 OK");
  else if ((strcmp(method, "PUT") == 0) && (strcmp(path, "/api/v1/config/can") == 0))
    HandleCanPut(connection, body);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/can/mode") == 0))
    HandleCanModeGet(connection, "200 OK");
  else if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/v1/can/mode") == 0))
    HandleCanModePost(connection, body);
  else if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/v1/can/nmt") == 0))
    HandleCanNmtPost(connection, body);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/can/tx") == 0))
    HandleCanTxStatus(connection);
  else if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/v1/can/tx") == 0))
    HandleCanTxPost(connection, body);
  else if ((strcmp(method, "GET") == 0) &&
           (strcmp(path, "/api/v1/can/tx/history") == 0))
    HandleCanTxHistory(connection);
  else if ((strcmp(method, "GET") == 0) &&
           (strcmp(path, "/api/v1/can/sdo/history") == 0))
    HandleSdoRequestHistory(connection);
  else if ((strcmp(method, "DELETE") == 0) &&
           (strcmp(path, "/api/v1/can/tx/periodic") == 0))
    HandleCanTxPeriodicStop(connection);
  else if ((strcmp(method, "POST") == 0) && ParseTxCancelPath(path, &tx_sequence))
    HandleCanTxCancel(connection, tx_sequence);
  else if ((strcmp(method, "POST") == 0) &&
           (strcmp(path, "/api/v1/bridge/enable") == 0))
    HandleBridgeEnable(connection);
  else if ((strcmp(method, "POST") == 0) &&
           (strcmp(path, "/api/v1/bridge/disable") == 0))
    HandleBridgeDisable(connection);
  else if ((strcmp(method, "GET") == 0) &&
           (strcmp(path, "/api/v1/bridge/status") == 0))
    WriteBridgeStatus(connection, "200 OK");
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/config/logger") == 0))
    WriteLoggerConfig(connection, "200 OK");
  else if ((strcmp(method, "PUT") == 0) && (strcmp(path, "/api/v1/config/logger") == 0))
    HandleLoggerPut(connection, body);
  else if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/v1/logger/start") == 0))
    HandleLoggerStart(connection);
  else if ((strcmp(method, "POST") == 0) && (strcmp(path, "/api/v1/logger/stop") == 0))
    HandleLoggerStop(connection);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/nodes") == 0))
    HandleNodes(connection, query);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/statistics") == 0))
    HandleStatistics(connection);
  else if ((strcmp(method, "POST") == 0) &&
           (strcmp(path, "/api/v1/statistics/reset") == 0))
    HandleStatisticsReset(connection);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/snapshots/frames") == 0))
    HandleFrames(connection, query);
  else if ((strcmp(method, "GET") == 0) &&
           ParseNodePath(path, "/events", &node_id))
    HandleNodeEvents(connection, node_id, false, query);
  else if ((strcmp(method, "GET") == 0) &&
           ParseNodePath(path, "/objects", &node_id))
    HandleNodeEvents(connection, node_id, true, query);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/logs") == 0))
    HandleLogs(connection, query);
  else if ((strcmp(method, "GET") == 0) &&
           ParseLogDownloadPath(path, log_path, log_filename))
    HandleLogDownload(connection, request, log_path, log_filename, true);
  else if ((strcmp(method, "HEAD") == 0) &&
           ParseLogDownloadPath(path, log_path, log_filename))
    HandleLogDownload(connection, request, log_path, log_filename, false);
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/stream") == 0))
  {
    bool transferred = HandleWebSocketUpgrade(connection, request);
    StatusIncrement(&public_status.completed_requests);
    return transferred;
  }
  else if ((strcmp(method, "GET") == 0) && (strcmp(path, "/api/v1/bridge") == 0))
  {
    bool transferred = HandleBridgeWebSocketUpgrade(connection, request, query);
    StatusIncrement(&public_status.completed_requests);
    return transferred;
  }
  else if ((strcmp(method, "GET") == 0) && (strncmp(path, "/api/", 5U) != 0))
    HandleWebAsset(connection, request, path, true);
  else if ((strcmp(method, "HEAD") == 0) && (strncmp(path, "/api/", 5U) != 0))
    HandleWebAsset(connection, request, path, false);
  else
  {
    SendError(connection, "404 Not Found", "not_found", "Unknown API endpoint");
  }
  StatusIncrement(&public_status.completed_requests);
  return false;
}

void ApiServer_Run(void)
{
  memset(&public_status, 0, sizeof(public_status));
  websocket_write_sequence = 0U;
  websocket_pending_connection = NULL;
  websocket_reserved = false;
  bridge_pending_connection = NULL;
  bridge_websocket_reserved = false;
  bridge_pending_session_id = 0U;
  bridge_stream_sequence = 0U;
  websocket_task_handle = xTaskCreateStatic(WebSocketTask, "wsLiveTask",
                                             WEBSOCKET_TASK_STACK_WORDS, NULL,
                                             WEBSOCKET_BASE_PRIORITY,
                                             websocket_task_stack,
                                             &websocket_task_control);
  bridge_task_handle = xTaskCreateStatic(BridgeWebSocketTask, "wsBridgeTask",
                                          BRIDGE_TASK_STACK_WORDS, NULL,
                                          tskIDLE_PRIORITY + 1U,
                                          bridge_task_stack,
                                          &bridge_task_control);
  for (;;)
  {
    struct netconn *listener;
    DeviceSettings settings;
    uint16_t bound_port;

    AppSettings_Get(&settings);
    bound_port = settings.network.http_port;
    listener = netconn_new(NETCONN_TCP);
    if (listener == NULL)
    {
      vTaskDelay(pdMS_TO_TICKS(1000U));
      continue;
    }
    netconn_set_recvtimeout(listener, 1000);
    if ((netconn_bind(listener, IP_ADDR_ANY, bound_port) != ERR_OK) ||
        (netconn_listen_with_backlog(listener, 6U) != ERR_OK))
    {
      netconn_delete(listener);
      vTaskDelay(pdMS_TO_TICKS(1000U));
      continue;
    }

    for (;;)
    {
      struct netconn *connection = NULL;
      if (netconn_accept(listener, &connection) == ERR_OK && connection != NULL)
      {
        size_t received = 0U;
        ApiRequestResult request_result;
        StatusIncrement(&public_status.accepted_connections);
        netconn_set_recvtimeout(connection, 5000);
        netconn_set_sendtimeout(connection, 1000);
        request_result = ReceiveRequest(connection, http_request_buffer,
                                        API_HTTP_REQUEST_BUFFER_BYTES, &received);
        if (request_result == API_REQUEST_OK)
        {
          bool transferred;
          (void)received;
          transferred = Dispatch(connection, http_request_buffer);
          if (transferred)
          {
            connection = NULL;
          }
        }
        else
        {
          StatusIncrement(&public_status.bad_requests);
          if (request_result == API_REQUEST_TOO_LARGE)
            SendError(connection, "431 Request Header Fields Too Large",
                      "request_too_large", "HTTP request exceeds 16 KiB");
          else
            SendError(connection, "408 Request Timeout", "request_incomplete",
                      "HTTP request was not received within 5 seconds");
        }
        if (connection != NULL)
        {
          (void)netconn_close(connection);
          netconn_delete(connection);
        }
      }
      CanBridge_Update();
      AppSettings_Get(&settings);
      if (settings.network.http_port != bound_port)
      {
        break;
      }
    }
    (void)netconn_close(listener);
    netconn_delete(listener);
  }
}

void ApiServer_SubmitFrame(const CanFrameRecord *frame)
{
  CanFrameRecord stream_frame;
  if (frame == NULL)
  {
    return;
  }
  CanBridge_ProcessFrame(frame, &stream_frame);
  taskENTER_CRITICAL();
  frame_history[frame_history_write] = *frame;
  frame_history_write = (frame_history_write + 1U) % API_FRAME_SNAPSHOT_MAX;
  if (frame_history_count < API_FRAME_SNAPSHOT_MAX)
  {
    ++frame_history_count;
  }
  websocket_ring[websocket_write_sequence & (WEBSOCKET_RING_CAPACITY - 1U)] =
    stream_frame;
  ++websocket_write_sequence;
  taskEXIT_CRITICAL();
}

void ApiServer_GetStatus(ApiServerStatus *status)
{
  if (status == NULL)
  {
    return;
  }
  taskENTER_CRITICAL();
  *status = public_status;
  taskEXIT_CRITICAL();
}
