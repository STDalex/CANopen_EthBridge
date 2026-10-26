#include "ui.h"

#include "app_version.h"
#include "app_settings.h"
#include "can_bridge.h"
#include "can_log.h"
#include "can_scanner.h"
#include "can_tx_manager.h"
#include "canopen_analyzer.h"
#include "can_pipeline.h"
#include "cmsis_os.h"
#include "dma2d.h"
#include "main.h"
#include "memory_layout.h"
#include "network_service.h"
#include "touch_ft5336.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_cyrillic_14)

#define LCD_WIDTH              480U
#define LCD_HEIGHT             272U
#define LCD_FRAMEBUFFER        0xC0000000UL
#define DISPLAY_IDLE_TIMEOUT_MS 180000U
#define UI_DRAW_LINES          24U
#define UI_VISIBLE_ROWS        12U
#define UI_HISTORY_LENGTH      256U
#define UI_HISTORY_MASK        (UI_HISTORY_LENGTH - 1U)
#define UI_ROW_HEIGHT          18U
#define UI_TABLE_Y             56U
#define UI_TABLE_HEIGHT        (UI_VISIBLE_ROWS * UI_ROW_HEIGHT)

#define UI_COL_TIME_X          4
#define UI_COL_ID_X            86
#define UI_COL_FORMAT_X        148
#define UI_COL_TYPE_X          186
#define UI_COL_LENGTH_X        236
#define UI_COL_DATA_X          262

#define UI_NODE_VISIBLE_ROWS   9U
#define UI_NODE_ROW_HEIGHT     23U
#define UI_NODE_TABLE_Y        59U
#define UI_NODE_TABLE_HEIGHT   (UI_NODE_VISIBLE_ROWS * UI_NODE_ROW_HEIGHT)

#define UI_EVENT_VISIBLE_ROWS  11U
#define UI_EVENT_ROW_HEIGHT    19U
#define UI_EVENT_TABLE_Y       59U
#define UI_EVENT_TABLE_HEIGHT  (UI_EVENT_VISIBLE_ROWS * UI_EVENT_ROW_HEIGHT)

#define UI_SCANNER_VISIBLE_ROWS 6U
#define UI_SCANNER_ROW_HEIGHT   24U
#define UI_SCANNER_TABLE_Y      82U
#define UI_SCANNER_TABLE_HEIGHT (UI_SCANNER_VISIBLE_ROWS * UI_SCANNER_ROW_HEIGHT)

#define UI_HISTORY_BYTES       (UI_HISTORY_LENGTH * sizeof(CanFrameRecord))
#define UI_NODE_SNAPSHOT_BYTES (CANOPEN_NODE_COUNT_MAX * sizeof(CanopenNodeSnapshot))
#define UI_EVENT_SNAPSHOT_BYTES \
  (CANOPEN_EVENT_SNAPSHOT_MAX * sizeof(CanopenNodeEventSnapshot))
#define UI_SCANNER_SNAPSHOT_BYTES \
  (CAN_SCANNER_RESULT_MAX * sizeof(CanScannerResult))

_Static_assert((UI_HISTORY_BYTES + UI_NODE_SNAPSHOT_BYTES + UI_EVENT_SNAPSHOT_BYTES +
                UI_SCANNER_SNAPSHOT_BYTES) <=
               UI_SNAPSHOT_REGION_BYTES,
               "UI snapshot buffers exceed their external SDRAM region");

typedef struct
{
  char time[12];
  char id[9];
  char format[4];
  char type[5];
  char length[2];
  char data[25];
} UiRowText;

typedef struct
{
  char node_id[6];
  char last_seen[12];
  char frames[11];
  char traffic[8];
  char response[8];
  char type[12];
} UiNodeRowText;

typedef struct
{
  char time[14];
  char latency[11];
  char id[9];
  char type[10];
  char object[5];
  char subindex[3];
  char data[25];
} UiEventRowText;

typedef struct
{
  char node_id[5];
  char bitrate[8];
  char response[20];
  char hint[30];
} UiScannerRowText;

typedef enum
{
  NODE_SORT_ID = 0,
  NODE_SORT_LAST_SEEN,
  NODE_SORT_FRAMES,
  NODE_SORT_TRAFFIC,
  NODE_SORT_RESPONSE,
  NODE_SORT_TYPE
} NodeSortKey;

typedef enum
{
  EVENT_SORT_TIME = 0,
  EVENT_SORT_LATENCY,
  EVENT_SORT_ID,
  EVENT_SORT_TYPE,
  EVENT_SORT_OBJECT,
  EVENT_SORT_SUBINDEX,
  EVENT_SORT_DATA
} EventSortKey;

typedef struct
{
  const char *name;
  const char *selector_text;
  uint32_t bitrate_kbps;
} UiBitrateOption;

static const UiBitrateOption bitrate_options[] =
{
  {"Auto", "Auto  " LV_SYMBOL_DOWN, 0U},
  {"1000", "1000 kbit/s  " LV_SYMBOL_DOWN, 1000U},
  {"800", "800 kbit/s  " LV_SYMBOL_DOWN, 800U},
  {"500", "500 kbit/s  " LV_SYMBOL_DOWN, 500U},
  {"250", "250 kbit/s  " LV_SYMBOL_DOWN, 250U},
  {"125", "125 kbit/s  " LV_SYMBOL_DOWN, 125U},
  {"100", "100 kbit/s  " LV_SYMBOL_DOWN, 100U},
  {"50", "50 kbit/s  " LV_SYMBOL_DOWN, 50U},
  {"20", "20 kbit/s  " LV_SYMBOL_DOWN, 20U},
  {"10", "10 kbit/s  " LV_SYMBOL_DOWN, 10U}
};

static const char *const tx_hex_keyboard_map[] =
{
  "0", "1", "2", "3", "4", "5", "6", "7", "\n",
  "8", "9", "A", "B", "C", "D", "E", "F", "\n",
  LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t tx_hex_keyboard_ctrl[] =
{
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 2, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};

static uint16_t draw_buffer[LCD_WIDTH * UI_DRAW_LINES] __attribute__((aligned(32)));
static uint32_t display_last_touch_ms;
static bool display_backlight_on;
static bool display_wake_touch_active;
static lv_obj_t *main_screen;
static lv_obj_t *monitor_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *network_screen;
static lv_obj_t *error_screen;
static lv_obj_t *nodes_screen;
static lv_obj_t *node_events_screen;
static lv_obj_t *info_screen;
static lv_obj_t *log_screen;
static lv_obj_t *tx_screen;
static lv_obj_t *scanner_screen;
static lv_obj_t *status_label;
static lv_obj_t *main_status_label;
static lv_obj_t *main_storage_label;
static lv_obj_t *pause_button_label;
static lv_obj_t *main_log_button;
static lv_obj_t *main_log_button_label;
static lv_obj_t *monitor_log_button;
static lv_obj_t *monitor_log_button_label;
static lv_obj_t *nodes_log_button;
static lv_obj_t *nodes_log_button_label;
static lv_obj_t *table_body;
static lv_obj_t *history_spacer;
static lv_obj_t *cell_labels[UI_VISIBLE_ROWS][6];
static UiRowText row_text[UI_VISIBLE_ROWS];
static CanFrameRecord *const history = (CanFrameRecord *)UI_SNAPSHOT_BASE;
static uint32_t history_write;
static uint32_t history_count;
static bool rows_dirty;
static bool display_paused;
static bool programmatic_scroll;
static lv_obj_t *nodes_title_label;
static lv_obj_t *nodes_back_button;
static lv_obj_t *nodes_back_button_label;
static lv_obj_t *nodes_return_screen;
static lv_obj_t *nodes_table_body;
static lv_obj_t *nodes_spacer;
static lv_obj_t *node_cell_labels[UI_NODE_VISIBLE_ROWS][6];
static lv_obj_t *node_open_buttons[UI_NODE_VISIBLE_ROWS];
static UiNodeRowText node_row_text[UI_NODE_VISIBLE_ROWS];
static CanopenNodeSnapshot *const node_snapshot =
  (CanopenNodeSnapshot *)(UI_SNAPSHOT_BASE + UI_HISTORY_BYTES);
static CanopenGlobalSnapshot analyzer_snapshot;
static uint32_t node_snapshot_count;
static NodeSortKey node_sort_key;
static bool node_sort_ascending;
static bool nodes_programmatic_scroll;
static lv_obj_t *node_events_title_label;
static lv_obj_t *node_events_table_body;
static lv_obj_t *node_events_spacer;
static lv_obj_t *node_events_pause_label;
static lv_obj_t *node_events_mode_label;
static lv_obj_t *node_events_type_header_label;
static lv_obj_t *nodes_response_header_label;
static lv_obj_t *event_cell_labels[UI_EVENT_VISIBLE_ROWS][7];
static UiEventRowText event_row_text[UI_EVENT_VISIBLE_ROWS];
static CanopenNodeEventSnapshot *const event_snapshot =
  (CanopenNodeEventSnapshot *)(UI_SNAPSHOT_BASE + UI_HISTORY_BYTES +
                               UI_NODE_SNAPSHOT_BYTES);
static uint32_t event_snapshot_count;
static uint8_t selected_node_id;
static EventSortKey event_sort_key;
static bool event_sort_ascending;
static bool events_programmatic_scroll;
static bool node_events_paused;
static bool node_object_mode;
static lv_obj_t *info_description_label;
static lv_obj_t *bitrate_selector_label;
static lv_obj_t *bitrate_options_panel;
static lv_obj_t *deep_sdo_button_label;
static bool deep_sdo_mode;
static lv_obj_t *network_dhcp_label;
static lv_obj_t *network_dhcp_switch;
static lv_obj_t *network_dhcp_state_label;
static lv_obj_t *network_ip_label;
static lv_obj_t *network_ip_textarea;
static lv_obj_t *network_mask_label;
static lv_obj_t *network_mask_textarea;
static lv_obj_t *network_gateway_label;
static lv_obj_t *network_gateway_textarea;
static lv_obj_t *network_save_button;
static lv_obj_t *network_edit_label;
static lv_obj_t *network_keyboard;
static lv_obj_t *network_status_label;
static lv_obj_t *network_active_field;
static int32_t network_active_field_y;
static lv_obj_t *error_summary_label;
static lv_obj_t *error_protocol_label;
static lv_obj_t *error_state_label;
static lv_obj_t *error_last_label;
static lv_obj_t *log_full_radio;
static lv_obj_t *log_nodes_radio;
static lv_obj_t *log_card_label;
static lv_obj_t *log_file_label;
static lv_obj_t *log_space_label;
static lv_obj_t *log_counters_label;
static lv_obj_t *log_selection_label;
static CanLogMode quick_log_mode = CAN_LOG_MODE_FULL;
static lv_obj_t *tx_nav_button;
static lv_obj_t *tx_send_button;
static lv_obj_t *tx_mode_button;
static lv_obj_t *tx_mode_label;
static lv_obj_t *tx_status_label;
static lv_obj_t *tx_node_textarea;
static lv_obj_t *tx_index_textarea;
static lv_obj_t *tx_subindex_textarea;
static lv_obj_t *tx_value_textarea;
static lv_obj_t *tx_period_textarea;
static lv_obj_t *tx_command_label;
static lv_obj_t *tx_edit_panel;
static lv_obj_t *tx_edit_label;
static lv_obj_t *tx_keyboard;
static lv_obj_t *tx_active_field;
static int32_t tx_field_x;
static int32_t tx_field_y;
static int32_t tx_field_width;
static int32_t tx_field_height;
static CanTxSdoCommand tx_command;
static lv_obj_t *bridge_indicator_label;
static lv_obj_t *scanner_indicator_label;
static lv_obj_t *scanner_action_button;
static lv_obj_t *scanner_action_label;
static lv_obj_t *scanner_status_label;
static lv_obj_t *scanner_table_body;
static lv_obj_t *scanner_spacer;
static lv_obj_t *scanner_cell_labels[UI_SCANNER_VISIBLE_ROWS][4];
static UiScannerRowText scanner_row_text[UI_SCANNER_VISIBLE_ROWS];
static CanScannerResult *const scanner_snapshot =
  (CanScannerResult *)(UI_SNAPSHOT_BASE + UI_HISTORY_BYTES +
                       UI_NODE_SNAPSHOT_BYTES + UI_EVENT_SNAPSHOT_BYTES);
static uint32_t scanner_snapshot_count;
static bool scanner_programmatic_scroll;

static const char info_monitor_text[] =
  "Монитор: все принятые CAN-кадры с временем, ID, форматом, DLC и данными. "
  "LOAD - оценка занятого времени CAN за последнюю секунду с учетом скорости, "
  "длины кадров и bit stuffing. Значение обновляется раз в секунду. "
  "Пауза не останавливает прием.";
static const char info_nodes_text[] =
  "Basic: SDO % - отношение ответов к запросам. Deep: OK % - доля завершенных "
  "операций среди OK, TO, AB и ERR. 100%! означает 100% успешных из однозначно "
  "завершенных, но также обнаружены AM или ORPH.";
static const char info_objects_text[] =
  "Deep/OBJ: OK - завершенная операция; TO - таймаут; AB - SDO Abort; "
  "AM - неоднозначная или перекрывающаяся операция. WAIT - ожидание ответа; "
  "ERR - ошибка протокола; ORPH - ответ без наблюдавшегося запроса.";
static const char info_bitrate_text[] =
  "Скорость: Auto перебирает поддерживаемые скорости. Ручной режим фиксирует "
  "выбранное значение. После LOCK передачу можно отдельно включить в разделе TX.";
static const char info_packet_types_text[] =
  "Типы: NMT - управление сетью; SYNC - синхронизация; EMCY - авария; "
  "TIME - время; TPDO - данные от ноды; RPDO - команды ноде; SDO - доступ к объектам; "
  "HB/Guard - контроль состояния; LSS - настройка ноды.";

static void Refresh_NodesSnapshot(void);
static void Refresh_NodeEventsSnapshot(void);
static void Render_Rows(void);
static void Render_NodeRows(void);
static void Refresh_LogStatus(void);
static void Tx_RefreshStatus(void);
static void Refresh_Scanner(void);

static void Refresh_ErrorStatistics(const CanCaptureStatus *status)
{
  char text[192];
  const CanErrorStatistics *errors;

  if (status == NULL)
  {
    return;
  }
  errors = &status->error_statistics;
  (void)snprintf(text, sizeof(text),
                 "Interrupts: %lu     CAN RX: %lu     DROP: %lu",
                 (unsigned long)errors->interrupts,
                 (unsigned long)status->received,
                 (unsigned long)(status->ring_overflows + status->gui_overflows +
                                 CanopenAnalyzer_GetQueueOverflows()));
  lv_label_set_text(error_summary_label, text);

  (void)snprintf(text, sizeof(text),
                 "PROTOCOL\nACK              %lu\nCRC              %lu\nStuff            %lu\n"
                 "Form             %lu\nBit recessive    %lu\nBit dominant     %lu",
                 (unsigned long)errors->ack,
                 (unsigned long)errors->crc,
                 (unsigned long)errors->stuff,
                 (unsigned long)errors->form,
                 (unsigned long)errors->bit_recessive,
                 (unsigned long)errors->bit_dominant);
  lv_label_set_text(error_protocol_label, text);

  (void)snprintf(text, sizeof(text),
                 "CONTROLLER\nWarning          %lu\nPassive          %lu\nBus-off          %lu\n"
                 "Unclassified     %lu",
                 (unsigned long)errors->warning,
                 (unsigned long)errors->passive,
                 (unsigned long)errors->bus_off,
                 (unsigned long)errors->unclassified);
  lv_label_set_text(error_state_label, text);

  (void)snprintf(text, sizeof(text),
                 "Last: %s at %lu.%03lu s    TEC:%u  REC:%u  ESR:0x%08lX",
                 CanControl_ErrorName(errors->last_error),
                 (unsigned long)(errors->last_timestamp_us / 1000000U),
                 (unsigned long)((errors->last_timestamp_us / 1000U) % 1000U),
                 (unsigned int)errors->last_tx_error_counter,
                 (unsigned int)errors->last_rx_error_counter,
                 (unsigned long)errors->last_esr);
  lv_label_set_text(error_last_label, text);
}

static const UiBitrateOption *Find_BitrateOption(uint32_t bitrate_kbps)
{
  uint32_t option;
  for (option = 0U; option < (sizeof(bitrate_options) / sizeof(bitrate_options[0])); ++option)
  {
    if (bitrate_options[option].bitrate_kbps == bitrate_kbps)
    {
      return &bitrate_options[option];
    }
  }
  return &bitrate_options[0];
}

static void Cache_Clean(const void *address, uint32_t size)
{
  uintptr_t start = (uintptr_t)address & ~(uintptr_t)31U;
  uintptr_t end = ((uintptr_t)address + size + 31U) & ~(uintptr_t)31U;
  SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static void Display_Flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
  uint32_t width = (uint32_t)(area->x2 - area->x1 + 1);
  uint32_t height = (uint32_t)(area->y2 - area->y1 + 1);
  uint16_t *destination = (uint16_t *)LCD_FRAMEBUFFER +
                          ((uint32_t)area->y1 * LCD_WIDTH) + (uint32_t)area->x1;

  Cache_Clean(pixels, width * height * sizeof(uint16_t));
  hdma2d.Init.Mode = DMA2D_M2M;
  hdma2d.Init.ColorMode = DMA2D_OUTPUT_RGB565;
  hdma2d.Init.OutputOffset = LCD_WIDTH - width;
  hdma2d.LayerCfg[1].InputOffset = 0U;
  hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
  hdma2d.LayerCfg[1].AlphaMode = DMA2D_NO_MODIF_ALPHA;
  hdma2d.LayerCfg[1].InputAlpha = 0xFFU;

  if ((HAL_DMA2D_Init(&hdma2d) != HAL_OK) ||
      (HAL_DMA2D_ConfigLayer(&hdma2d, 1U) != HAL_OK) ||
      (HAL_DMA2D_Start(&hdma2d, (uint32_t)pixels, (uint32_t)destination, width, height) != HAL_OK) ||
      (HAL_DMA2D_PollForTransfer(&hdma2d, 20U) != HAL_OK))
  {
    uint32_t line;
    uint16_t *source = (uint16_t *)pixels;
    for (line = 0U; line < height; ++line)
    {
      memcpy(destination + (line * LCD_WIDTH), source + (line * width), width * sizeof(uint16_t));
    }
  }

  lv_display_flush_ready(display);
}

static void Display_SetBacklight(bool enabled)
{
  HAL_GPIO_WritePin(LCD_BL_CTRL_GPIO_Port, LCD_BL_CTRL_Pin,
                    enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
  display_backlight_on = enabled;
}

static void Touch_Read(lv_indev_t *indev, lv_indev_data_t *data)
{
  uint16_t x = 0U;
  uint16_t y = 0U;
  bool pressed = false;
  (void)indev;

  (void)TouchFT5336_Read(&x, &y, &pressed);
  if (pressed)
  {
    display_last_touch_ms = HAL_GetTick();
    if (!display_backlight_on)
    {
      Display_SetBacklight(true);
      display_wake_touch_active = true;
    }
  }

  data->point.x = (int32_t)x;
  data->point.y = (int32_t)y;
  data->state = (pressed && !display_wake_touch_active) ?
                LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (!pressed)
  {
    display_wake_touch_active = false;
  }
}

static void Bitrate_SelectorClicked(lv_event_t *event)
{
  (void)event;
  if (lv_obj_is_hidden(bitrate_options_panel))
  {
    lv_obj_set_hidden(bitrate_options_panel, false);
    lv_obj_move_foreground(bitrate_options_panel);
  }
  else
  {
    lv_obj_set_hidden(bitrate_options_panel, true);
  }
  lv_obj_invalidate(settings_screen);
}

static void Bitrate_OptionClicked(lv_event_t *event)
{
  const UiBitrateOption *option =
    (const UiBitrateOption *)lv_event_get_user_data(event);
  CanScannerStatus scanner_status;

  if (option != NULL)
  {
    CanScanner_GetStatus(&scanner_status);
    if (scanner_status.running) CanScanner_Stop();
    lv_label_set_text(bitrate_selector_label, option->selector_text);
    CanControl_RequestBitrate(option->bitrate_kbps);
    (void)AppSettings_UpdateCan(option->bitrate_kbps, deep_sdo_mode, true);
  }
  lv_obj_set_hidden(bitrate_options_panel, true);
  lv_obj_invalidate(settings_screen);
}

static void DeepSdo_ModeClicked(lv_event_t *event)
{
  (void)event;
  deep_sdo_mode = !deep_sdo_mode;
  {
    DeviceSettings settings;
    AppSettings_Get(&settings);
    (void)AppSettings_UpdateCan(settings.can_bitrate_kbps, deep_sdo_mode, true);
  }
  lv_label_set_text(deep_sdo_button_label, deep_sdo_mode ? "[X] Deep" : "[ ] Basic");
  lv_label_set_text(nodes_response_header_label, deep_sdo_mode ? "OK %" : "SDO %");
  lv_label_set_text(node_events_type_header_label,
                    (deep_sdo_mode && node_object_mode) ? "State" : "Type");
  if (lv_screen_active() == nodes_screen)
  {
    Refresh_NodesSnapshot();
  }
  else if ((lv_screen_active() == node_events_screen) && (selected_node_id != 0U))
  {
    Refresh_NodeEventsSnapshot();
  }
}

static void Settings_ScreenLoaded(lv_event_t *event)
{
  DeviceSettings settings;
  const UiBitrateOption *option;
  (void)event;

  AppSettings_Get(&settings);
  option = Find_BitrateOption(settings.can_bitrate_kbps);
  deep_sdo_mode = settings.deep_sdo_enabled;
  lv_label_set_text(bitrate_selector_label, option->selector_text);
  lv_label_set_text(deep_sdo_button_label, deep_sdo_mode ? "[X] Deep" : "[ ] Basic");
}

static void Network_UpdateDhcpLabel(void)
{
  lv_label_set_text(network_dhcp_state_label,
                    lv_obj_has_state(network_dhcp_switch, LV_STATE_CHECKED) ? "ON" : "OFF");
}

static void Network_DhcpChanged(lv_event_t *event)
{
  (void)event;
  Network_UpdateDhcpLabel();
}

static void Network_EndEditing(void)
{
  if (network_active_field != NULL)
  {
    lv_obj_set_y(network_active_field, network_active_field_y);
    network_active_field = NULL;
  }

  lv_keyboard_set_textarea(network_keyboard, NULL);
  lv_obj_set_hidden(network_keyboard, true);
  lv_obj_set_hidden(network_edit_label, true);
  lv_obj_set_hidden(network_dhcp_label, false);
  lv_obj_set_hidden(network_dhcp_switch, false);
  lv_obj_set_hidden(network_dhcp_state_label, false);
  lv_obj_set_hidden(network_ip_label, false);
  lv_obj_set_hidden(network_ip_textarea, false);
  lv_obj_set_hidden(network_mask_label, false);
  lv_obj_set_hidden(network_mask_textarea, false);
  lv_obj_set_hidden(network_gateway_label, false);
  lv_obj_set_hidden(network_gateway_textarea, false);
  lv_obj_set_hidden(network_save_button, false);
  lv_obj_set_hidden(network_status_label, false);
}

static void Network_ScreenLoaded(lv_event_t *event)
{
  DeviceSettings settings;
  char text[20];
  (void)event;

  Network_EndEditing();
  AppSettings_Get(&settings);
  if (settings.network.dhcp_enabled)
  {
    lv_obj_add_state(network_dhcp_switch, LV_STATE_CHECKED);
  }
  else
  {
    lv_obj_remove_state(network_dhcp_switch, LV_STATE_CHECKED);
  }
  Network_UpdateDhcpLabel();
  DeviceSettings_FormatIpv4(&settings.network.static_ip, text, sizeof(text));
  lv_textarea_set_text(network_ip_textarea, text);
  DeviceSettings_FormatIpv4(&settings.network.netmask, text, sizeof(text));
  lv_textarea_set_text(network_mask_textarea, text);
  DeviceSettings_FormatIpv4(&settings.network.gateway, text, sizeof(text));
  lv_textarea_set_text(network_gateway_textarea, text);
  lv_label_set_text(network_status_label, "Edit and press Save");
}

static void Network_FieldClicked(lv_event_t *event)
{
  lv_obj_t *textarea = lv_event_get_target_obj(event);

  if (network_active_field != textarea)
  {
    Network_EndEditing();
    network_active_field = textarea;
    network_active_field_y = lv_obj_get_y(textarea);
  }

  if (textarea == network_ip_textarea)
  {
    lv_label_set_text(network_edit_label, "Static IP");
  }
  else if (textarea == network_mask_textarea)
  {
    lv_label_set_text(network_edit_label, "Netmask");
  }
  else
  {
    lv_label_set_text(network_edit_label, "Gateway");
  }

  lv_obj_set_hidden(network_dhcp_label, true);
  lv_obj_set_hidden(network_dhcp_switch, true);
  lv_obj_set_hidden(network_dhcp_state_label, true);
  lv_obj_set_hidden(network_ip_label, true);
  lv_obj_set_hidden(network_mask_label, true);
  lv_obj_set_hidden(network_gateway_label, true);
  lv_obj_set_hidden(network_ip_textarea, textarea != network_ip_textarea);
  lv_obj_set_hidden(network_mask_textarea, textarea != network_mask_textarea);
  lv_obj_set_hidden(network_gateway_textarea, textarea != network_gateway_textarea);
  lv_obj_set_hidden(network_save_button, true);
  lv_obj_set_hidden(network_status_label, true);
  lv_obj_set_hidden(network_edit_label, false);
  lv_obj_set_pos(textarea, 126, 43);
  lv_keyboard_set_textarea(network_keyboard, textarea);
  lv_obj_set_hidden(network_keyboard, false);
  lv_obj_move_foreground(network_keyboard);
}

static void Network_KeyboardEvent(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if ((code == LV_EVENT_READY) || (code == LV_EVENT_CANCEL))
  {
    Network_EndEditing();
  }
}

static void Network_ScreenUnloaded(lv_event_t *event)
{
  (void)event;
  Network_EndEditing();
}

static void Network_SaveClicked(lv_event_t *event)
{
  DeviceSettings settings;
  NetworkSettings network;
  (void)event;

  AppSettings_Get(&settings);
  network = settings.network;
  network.dhcp_enabled = lv_obj_has_state(network_dhcp_switch, LV_STATE_CHECKED);
  if (!DeviceSettings_ParseIpv4(lv_textarea_get_text(network_ip_textarea), &network.static_ip) ||
      !DeviceSettings_ParseIpv4(lv_textarea_get_text(network_mask_textarea), &network.netmask) ||
      !DeviceSettings_ParseIpv4(lv_textarea_get_text(network_gateway_textarea), &network.gateway) ||
      !DeviceSettings_IsValidNetwork(&network))
  {
    lv_label_set_text(network_status_label, "Invalid IP configuration");
    lv_obj_set_style_text_color(network_status_label, lv_color_hex(0xFF7A7A), 0);
    return;
  }
  if (!AppSettings_UpdateNetwork(&network, true))
  {
    lv_label_set_text(network_status_label, "QSPI write failed");
    lv_obj_set_style_text_color(network_status_label, lv_color_hex(0xFF7A7A), 0);
    return;
  }
  NetworkService_RequestApply();
  lv_label_set_text(network_status_label, "Saved; network is reconfiguring");
  lv_obj_set_style_text_color(network_status_label, lv_color_hex(0x65C7F7), 0);
}

static bool Tx_ParseField(lv_obj_t *field, uint32_t base, uint32_t maximum,
                          uint32_t *value)
{
  const char *text = lv_textarea_get_text(field);
  char *end;
  unsigned long parsed;
  if ((text == NULL) || (*text == '\0') || (value == NULL)) return false;
  parsed = strtoul(text, &end, (int)base);
  if ((end == text) || (*end != '\0') || (parsed > maximum)) return false;
  *value = (uint32_t)parsed;
  return true;
}

static void Tx_StyleField(lv_obj_t *field)
{
  lv_obj_set_style_bg_color(field, lv_color_hex(0x17242B), 0);
  lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(field, lv_color_hex(0x38566A), 0);
  lv_obj_set_style_border_width(field, 1, 0);
  lv_obj_set_style_radius(field, 0, 0);
  lv_obj_set_style_text_color(field, lv_color_hex(0xF4FAFD), 0);
  lv_obj_set_style_pad_left(field, 10, 0);
  lv_obj_set_style_pad_right(field, 8, 0);
  lv_obj_set_style_bg_color(field, lv_color_hex(0x10181E), LV_STATE_DISABLED);
  lv_obj_set_style_text_color(field, lv_color_hex(0x78909C), LV_STATE_DISABLED);
  lv_obj_set_style_border_color(field, lv_color_hex(0x263842), LV_STATE_DISABLED);
  lv_obj_set_style_opa(field, LV_OPA_COVER, LV_STATE_DISABLED);
}

static void Tx_UpdateFormLabels(void)
{
  const char *label;
  uint32_t maximum_length;
  switch (tx_command)
  {
    case CAN_TX_SDO_READ: label = "READ"; maximum_length = 8U; break;
    case CAN_TX_SDO_WRITE_U8: label = "WRITE 8"; maximum_length = 2U; break;
    case CAN_TX_SDO_WRITE_U16: label = "WRITE 16"; maximum_length = 4U; break;
    case CAN_TX_SDO_WRITE_U24: label = "WRITE 24"; maximum_length = 6U; break;
    default: label = "WRITE 32"; maximum_length = 8U; break;
  }
  lv_label_set_text(tx_command_label, label);
  lv_textarea_set_max_length(tx_value_textarea, maximum_length);
  if (tx_command == CAN_TX_SDO_READ)
  {
    lv_obj_add_state(tx_value_textarea, LV_STATE_DISABLED);
  }
  else
  {
    lv_obj_remove_state(tx_value_textarea, LV_STATE_DISABLED);
  }
}

static void Tx_EndEditing(void)
{
  if (tx_active_field != NULL)
  {
    lv_obj_set_pos(tx_active_field, tx_field_x, tx_field_y);
    lv_obj_set_size(tx_active_field, tx_field_width, tx_field_height);
  }
  tx_active_field = NULL;
  lv_keyboard_set_textarea(tx_keyboard, NULL);
  lv_obj_set_hidden(tx_keyboard, true);
  lv_obj_set_hidden(tx_edit_panel, true);
}

static const char *Tx_FieldName(lv_obj_t *field)
{
  if (field == tx_node_textarea) return "Node";
  if (field == tx_index_textarea) return "Object";
  if (field == tx_subindex_textarea) return "Sub";
  if (field == tx_value_textarea) return "Value";
  if (field == tx_period_textarea) return "Period";
  return "Value";
}

static void Tx_BeginEditing(lv_obj_t *field)
{
  if (lv_obj_has_state(field, LV_STATE_DISABLED)) return;
  if ((tx_active_field != NULL) && (tx_active_field != field)) Tx_EndEditing();
  tx_field_x = lv_obj_get_x(field);
  tx_field_y = lv_obj_get_y(field);
  tx_field_width = lv_obj_get_width(field);
  tx_field_height = lv_obj_get_height(field);
  tx_active_field = field;
  lv_label_set_text(tx_edit_label, Tx_FieldName(field));
  lv_obj_set_hidden(tx_edit_panel, false);
  lv_obj_set_pos(field, 112, 41);
  lv_obj_set_size(field, 360, 38);
  lv_keyboard_set_mode(tx_keyboard,
                       (field == tx_node_textarea || field == tx_period_textarea) ?
                       LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_USER_1);
  lv_keyboard_set_textarea(tx_keyboard, field);
  lv_obj_set_hidden(tx_keyboard, false);
  lv_obj_move_foreground(tx_edit_panel);
  lv_obj_move_foreground(field);
  lv_obj_move_foreground(tx_keyboard);
}

static void Tx_FieldClicked(lv_event_t *event)
{
  Tx_BeginEditing(lv_event_get_target_obj(event));
}

static void Tx_KeyboardEvent(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if ((code == LV_EVENT_READY) || (code == LV_EVENT_CANCEL)) Tx_EndEditing();
}

static void Tx_CommandClicked(lv_event_t *event)
{
  (void)event;
  tx_command = (CanTxSdoCommand)(((uint32_t)tx_command + 1U) % 5U);
  Tx_UpdateFormLabels();
}

static bool Tx_ReadRequest(CanTxSdoRequest *request)
{
  uint32_t value;
  if (request == NULL) return false;
  memset(request, 0, sizeof(*request));
  request->command = tx_command;
  request->timeout_ms = CAN_TX_DEFAULT_TIMEOUT_MS;
  if (!Tx_ParseField(tx_node_textarea, 10U, 127U, &value) || (value == 0U))
  {
    lv_label_set_text(tx_status_label, "Node ID must be 1..127 (decimal)");
    return false;
  }
  request->node_id = (uint8_t)value;
  if (!Tx_ParseField(tx_index_textarea, 16U, 0xFFFFU, &value))
  {
    lv_label_set_text(tx_status_label, "Object index must be 0000..FFFF (hex)");
    return false;
  }
  request->index = (uint16_t)value;
  if (!Tx_ParseField(tx_subindex_textarea, 16U, 0xFFU, &value))
  {
    lv_label_set_text(tx_status_label, "Subindex must be 00..FF (hex)");
    return false;
  }
  request->subindex = (uint8_t)value;
  if (tx_command != CAN_TX_SDO_READ)
  {
    if (!Tx_ParseField(tx_value_textarea, 16U, 0xFFFFFFFFUL, &request->value))
    {
      lv_label_set_text(tx_status_label, "Value does not fit selected write size");
      return false;
    }
  }
  return true;
}

static void Tx_ModeClicked(lv_event_t *event)
{
  CanTxStatus status;
  CanTxSdoRequest request;
  CanTxSubmitResult result;
  uint32_t period_ms;
  uint32_t sequence;
  char text[96];
  CanScannerStatus scanner_status;
  (void)event;
  CanScanner_GetStatus(&scanner_status);
  if (scanner_status.running)
  {
    lv_label_set_text(tx_status_label, "CAN scanner owns transmission");
    return;
  }
  if (CanBridge_IsActive())
  {
    lv_label_set_text(tx_status_label, "Bridge mode: local transmission is disabled");
    return;
  }
  CanTx_GetStatus(&status);
  if (status.periodic_active)
  {
    (void)CanTx_StopPeriodic();
    lv_label_set_text(tx_status_label, "Periodic SDO stopping; returning to safe receive");
    return;
  }
  if (!Tx_ReadRequest(&request)) return;
  if (!Tx_ParseField(tx_period_textarea, 10U, CAN_TX_PERIOD_MAX_MS, &period_ms) ||
      (period_ms < CAN_TX_PERIOD_MIN_MS) || (period_ms < request.timeout_ms))
  {
    lv_label_set_text(tx_status_label, "Period must be 50..3600000 ms and >= timeout");
    return;
  }
  result = CanTx_StartPeriodicSdo(&request, period_ms, &sequence);
  if (result == CAN_TX_SUBMIT_OK)
  {
    (void)snprintf(text, sizeof(text), "Periodic #%lu started every %lu ms",
                   (unsigned long)sequence, (unsigned long)period_ms);
    lv_label_set_text(tx_status_label, text);
  }
  else if (result == CAN_TX_SUBMIT_MODE_DISABLED)
    lv_label_set_text(tx_status_label, "Wait for bitrate LOCK or select it manually");
  else if (result == CAN_TX_SUBMIT_QUEUE_FULL)
    lv_label_set_text(tx_status_label, "Periodic SDO is already active");
  else
    lv_label_set_text(tx_status_label, "Invalid period or SDO request");
}

static void Tx_SendClicked(lv_event_t *event)
{
  CanTxSdoRequest request;
  CanTxSubmitResult result;
  uint32_t sequence;
  char text[96];
  CanScannerStatus scanner_status;
  (void)event;
  CanScanner_GetStatus(&scanner_status);
  if (scanner_status.running)
  {
    lv_label_set_text(tx_status_label, "CAN scanner owns transmission");
    return;
  }
  if (CanBridge_IsActive())
  {
    lv_label_set_text(tx_status_label, "Bridge mode: local transmission is disabled");
    return;
  }
  if (!Tx_ReadRequest(&request)) return;
  result = CanTx_SubmitSdo(&request, &sequence);
  if (result == CAN_TX_SUBMIT_OK)
  {
    (void)snprintf(text, sizeof(text),
                   "Queued #%lu: node %u, %s 0x%04X:%02X",
                   (unsigned long)sequence, (unsigned int)request.node_id,
                   CanTx_SdoCommandName(request.command),
                   (unsigned int)request.index, (unsigned int)request.subindex);
    lv_label_set_text(tx_status_label, text);
  }
  else if (result == CAN_TX_SUBMIT_MODE_DISABLED)
  {
    lv_label_set_text(tx_status_label, "Wait for bitrate LOCK or select it manually");
  }
  else if (result == CAN_TX_SUBMIT_QUEUE_FULL)
  {
    lv_label_set_text(tx_status_label, "TX queue full (16 frames)");
  }
  else
  {
    lv_label_set_text(tx_status_label, "Invalid SDO request or value size");
  }
}

static void Tx_ScreenLoaded(lv_event_t *event)
{
  (void)event;
  Tx_EndEditing();
  lv_label_set_text(tx_status_label, "");
  Tx_RefreshStatus();
}

static void Tx_RefreshStatus(void)
{
  CanTxStatus tx;
  CanScannerStatus scanner_status;
  bool bridge_active = CanBridge_IsActive();
  bool scanner_active;
  CanScanner_GetStatus(&scanner_status);
  scanner_active = scanner_status.running;
  CanTx_GetStatus(&tx);
  lv_label_set_text(tx_mode_label,
                    scanner_active ? "SCANNER CONTROLLED" :
                    (bridge_active ? "BRIDGE CONTROLLED" :
                    (tx.periodic_active ? "STOP PERIODIC SEND" : "START PERIODIC SEND")));
  if (scanner_active || bridge_active)
  {
    lv_obj_add_state(tx_send_button, LV_STATE_DISABLED);
    lv_obj_add_state(tx_mode_button, LV_STATE_DISABLED);
    lv_label_set_text(tx_status_label,
                      scanner_active ? "CAN scanner owns transmission" :
                      "Bridge mode: SDO requests are accepted only via Ethernet");
  }
  else
  {
    lv_obj_remove_state(tx_send_button, LV_STATE_DISABLED);
    lv_obj_remove_state(tx_mode_button, LV_STATE_DISABLED);
  }
  lv_obj_set_style_bg_color(lv_obj_get_child(tx_mode_button, 0),
                            lv_color_hex((scanner_active || bridge_active || tx.periodic_active) ?
                                         0x6D28D9 : 0x17698C), 0);
  lv_obj_set_style_bg_color(lv_obj_get_child(tx_send_button, 0),
                            lv_color_hex((scanner_active || bridge_active) ?
                                         0x3F3F46 : 0x17698C), 0);
  lv_obj_set_style_bg_color(lv_obj_get_child(tx_nav_button, 0),
                            lv_color_hex((scanner_active || bridge_active || tx.periodic_active) ?
                                         0x6D28D9 : 0x17698C), 0);
  lv_obj_set_style_text_color(tx_status_label,
                              lv_color_hex((scanner_active || bridge_active || tx.periodic_active) ?
                                           0xC084FC : 0xA9C7D7), 0);
}

static void Log_ModeSelected(lv_event_t *event)
{
  CanLogMode selected_mode = (CanLogMode)(uintptr_t)lv_event_get_user_data(event);
  CanLogMode active_mode = CanLogger_GetMode();

  if ((selected_mode != CAN_LOG_MODE_FULL) &&
      (selected_mode != CAN_LOG_MODE_SELECTED))
  {
    return;
  }

  quick_log_mode = selected_mode;
  if (active_mode != CAN_LOG_MODE_OFF)
  {
    CanLogger_SetMode(selected_mode);
  }
  Refresh_LogStatus();
}

static void Quick_LogClicked(lv_event_t *event)
{
  CanLogMode mode = CanLogger_GetMode();
  (void)event;

  if (mode == CAN_LOG_MODE_OFF)
  {
    CanLogger_SetMode(quick_log_mode);
  }
  else
  {
    quick_log_mode = mode;
    CanLogger_SetMode(CAN_LOG_MODE_OFF);
  }
  Refresh_LogStatus();
}

static void Log_ClearNodesClicked(lv_event_t *event)
{
  (void)event;
  CanLogger_ClearSelectedNodes();
  Render_NodeRows();
  Refresh_LogStatus();
}

static void Reset_StatisticsClicked(lv_event_t *event)
{
  (void)event;
  CanPipeline_ResetStatistics();

  history_write = 0U;
  history_count = 0U;
  node_snapshot_count = 0U;
  event_snapshot_count = 0U;
  selected_node_id = 0U;
  memset(&analyzer_snapshot, 0, sizeof(analyzer_snapshot));

  programmatic_scroll = true;
  lv_obj_scroll_to_y(table_body, 0, LV_ANIM_OFF);
  programmatic_scroll = false;
  lv_obj_set_height(history_spacer, UI_TABLE_HEIGHT);
  Render_Rows();
  rows_dirty = false;
  lv_label_set_text(main_status_label, "Statistics reset");
}

static void Style_Screen(lv_obj_t *screen)
{
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
  lv_obj_set_style_text_color(screen, lv_color_hex(0xE8EEF2), 0);
  lv_obj_set_style_text_font(screen, &lv_font_montserrat_12, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
}

static void Navigation_Clicked(lv_event_t *event)
{
  lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(event);
  lv_obj_t *source = lv_screen_active();

  if ((lv_event_get_current_target_obj(event) == nodes_back_button) &&
      (nodes_return_screen != NULL))
  {
    target = nodes_return_screen;
  }
  if (target != NULL)
  {
    if (target == monitor_screen)
    {
      rows_dirty = true;
    }
    else if (target == nodes_screen)
    {
      if (source == log_screen)
      {
        nodes_return_screen = log_screen;
        lv_label_set_text(nodes_back_button_label, LV_SYMBOL_LEFT);
      }
      else if (source == main_screen)
      {
        nodes_return_screen = main_screen;
        lv_label_set_text(nodes_back_button_label, LV_SYMBOL_HOME);
      }
      Refresh_NodesSnapshot();
    }
    else if (target == log_screen)
    {
      Refresh_LogStatus();
    }
    else if (target == tx_screen)
    {
      Tx_RefreshStatus();
    }
    lv_screen_load(target);
  }
}

static lv_obj_t *Create_NavigationButton(lv_obj_t *parent, int32_t x, int32_t y,
                                         int32_t width, int32_t height,
                                         const char *text, lv_obj_t *target)
{
  lv_obj_t *button = lv_obj_create(parent);
  lv_obj_t *background;
  lv_obj_t *label;

  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_clickable(button, true);
  lv_obj_set_scrollable(button, false);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x17698C), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x238DB8), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_add_event_cb(button, Navigation_Clicked, LV_EVENT_CLICKED, target);

  background = lv_obj_create(button);
  lv_obj_remove_style_all(background);
  lv_obj_set_pos(background, 0, 0);
  lv_obj_set_size(background, width, height);
  lv_obj_set_style_bg_color(background, lv_color_hex(0x17698C), 0);
  lv_obj_set_style_bg_opa(background, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(background, 1, 0);
  lv_obj_set_style_border_color(background, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_clickable(background, false);
  lv_obj_set_scrollable(background, false);

  label = lv_label_create(button);
  lv_obj_set_width(label, width - 12);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

static lv_obj_t *Create_QuickLogButton(lv_obj_t *parent, int32_t x,
                                       lv_obj_t **label)
{
  lv_obj_t *button = lv_button_create(parent);

  lv_obj_set_pos(button, x, 1);
  lv_obj_set_size(button, 76, 34);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x17698C), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x238DB8), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_radius(button, 5, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_add_event_cb(button, Quick_LogClicked, LV_EVENT_CLICKED, NULL);

  *label = lv_label_create(button);
  lv_label_set_text(*label, "LOG " LV_SYMBOL_PLAY);
  lv_obj_center(*label);
  return button;
}

static lv_obj_t *Create_LogModeRadio(lv_obj_t *parent, int32_t x,
                                     const char *text, CanLogMode mode)
{
  lv_obj_t *radio = lv_checkbox_create(parent);

  lv_obj_set_pos(radio, x, 55);
  lv_obj_set_size(radio, 86, 32);
  lv_checkbox_set_text_static(radio, text);
  lv_obj_set_style_text_color(radio, lv_color_hex(0xE8EEF2), 0);
  lv_obj_set_style_pad_column(radio, 6, 0);
  lv_obj_set_style_radius(radio, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(radio, 2, LV_PART_INDICATOR);
  lv_obj_set_style_border_color(radio, lv_color_hex(0x65C7F7), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(radio, lv_color_hex(0x101418), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(radio, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(radio, lv_color_hex(0x238DB8),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(radio, lv_color_hex(0xA6E2FF),
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(radio, Log_ModeSelected, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)mode);
  return radio;
}

static void Info_ModeClicked(lv_event_t *event)
{
  const char *description = (const char *)lv_event_get_user_data(event);
  if (description != NULL)
  {
    lv_label_set_text(info_description_label, description);
  }
}

static lv_obj_t *Create_InfoModeButton(lv_obj_t *parent, int32_t x, int32_t width,
                                       const char *text,
                                       const char *description)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_pos(button, x, 100);
  lv_obj_set_size(button, width, 38);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x17698C), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x238DB8), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_radius(button, 6, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_add_event_cb(button, Info_ModeClicked, LV_EVENT_CLICKED, (void *)description);

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return button;
}

static lv_obj_t *Create_Label(lv_obj_t *parent, int32_t x, int32_t y, int32_t width,
                              const char *text, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_label_set_text(label, text);
  return label;
}

static void Create_GridLine(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height)
{
  lv_obj_t *line = lv_obj_create(parent);
  lv_obj_remove_style_all(line);
  lv_obj_set_pos(line, x, y);
  lv_obj_set_size(line, width, height);
  lv_obj_set_style_bg_color(line, lv_color_hex(0x31424C), 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_set_clickable(line, false);
  lv_obj_set_scrollable(line, false);
}

static lv_obj_t *Create_TableHeader(lv_obj_t *parent, int32_t x, int32_t width,
                                    const char *text, lv_event_cb_t callback,
                                    uintptr_t key)
{
  lv_obj_t *button = lv_obj_create(parent);
  lv_obj_t *label;

  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, 0);
  lv_obj_set_size(button, width, 22);
  lv_obj_set_clickable(button, callback != NULL);
  lv_obj_set_scrollable(button, false);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x21343E), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
  if (callback != NULL)
  {
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)key);
  }

  label = lv_label_create(button);
  lv_obj_set_width(label, width - 4);
  lv_obj_set_style_text_color(label, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, 2, 3);
  return button;
}

static void Set_DisplayPaused(bool paused)
{
  display_paused = paused;
  lv_label_set_text(pause_button_label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  rows_dirty = true;
}

static void Format_Row(UiRowText *text, const CanFrameRecord *frame)
{
  uint32_t i;
  uint32_t offset = 0U;

  (void)snprintf(text->time, sizeof(text->time), "%5lu.%05lu",
                 (unsigned long)(frame->timestamp_us / 1000000U),
                 (unsigned long)((frame->timestamp_us / 10U) % 100000U));
  (void)snprintf(text->id, sizeof(text->id),
                 ((frame->flags & CAN_FRAME_FLAG_EXTENDED) != 0U) ? "%08lX" : "%03lX",
                 (unsigned long)frame->id);
  (void)snprintf(text->format, sizeof(text->format), "%s",
                 ((frame->flags & CAN_FRAME_FLAG_EXTENDED) != 0U) ? "EXT" : "STD");
  (void)snprintf(text->type, sizeof(text->type), "%s",
                 ((frame->flags & CAN_FRAME_FLAG_LOCAL_TX) != 0U) ? "TX" :
                 (((frame->flags & CAN_FRAME_FLAG_REMOTE) != 0U) ? "RTR" : "DATA"));
  (void)snprintf(text->length, sizeof(text->length), "%u", frame->dlc);
  text->data[0] = '\0';

  if ((frame->flags & CAN_FRAME_FLAG_REMOTE) == 0U)
  {
    for (i = 0U; (i < frame->dlc) && (offset < (sizeof(text->data) - 3U)); ++i)
    {
      offset += (uint32_t)snprintf(&text->data[offset], sizeof(text->data) - offset,
                                   (i == 0U) ? "%02X" : " %02X", frame->data[i]);
    }
  }
}

static void Clear_Row(UiRowText *text)
{
  text->time[0] = '\0';
  text->id[0] = '\0';
  text->format[0] = '\0';
  text->type[0] = '\0';
  text->length[0] = '\0';
  text->data[0] = '\0';
}

static void Render_Rows(void)
{
  static const uint32_t normal_colors[6] =
  {
    0xE8EEF2U, 0xE8EEF2U, 0xA9C7D7U,
    0xA9C7D7U, 0xA9C7D7U, 0xE8EEF2U
  };
  int32_t scroll_y = lv_obj_get_scroll_y(table_body);
  uint32_t first_age;
  uint32_t row;

  if (scroll_y < 0)
  {
    scroll_y = 0;
  }
  first_age = (uint32_t)scroll_y / UI_ROW_HEIGHT;

  for (row = 0U; row < UI_VISIBLE_ROWS; ++row)
  {
    uint32_t age = first_age + row;
    int32_t row_y = (int32_t)(age * UI_ROW_HEIGHT) + 2;
    uint32_t column;

    if (age < history_count)
    {
      uint32_t index = (history_write - 1U - age) & UI_HISTORY_MASK;
      Format_Row(&row_text[row], &history[index]);
    }
    else
    {
      Clear_Row(&row_text[row]);
    }

    for (column = 0U; column < 6U; ++column)
    {
      bool local_tx = (age < history_count) &&
        ((history[(history_write - 1U - age) & UI_HISTORY_MASK].flags &
          CAN_FRAME_FLAG_LOCAL_TX) != 0U);
      lv_obj_set_y(cell_labels[row][column], row_y);
      lv_obj_set_style_text_color(cell_labels[row][column],
                                  lv_color_hex(local_tx ? 0xC084FCU :
                                               normal_colors[column]), 0);
      lv_obj_invalidate(cell_labels[row][column]);
    }
  }
}

static void Pause_ButtonClicked(lv_event_t *event)
{
  (void)event;
  if (display_paused)
  {
    programmatic_scroll = true;
    lv_obj_scroll_to_y(table_body, 0, LV_ANIM_OFF);
    programmatic_scroll = false;
    Set_DisplayPaused(false);
  }
  else
  {
    Set_DisplayPaused(true);
  }
}

static void Table_Scrolled(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);

  if ((code == LV_EVENT_SCROLL_BEGIN) && !programmatic_scroll)
  {
    Set_DisplayPaused(true);
  }
  else if (code == LV_EVENT_SCROLL)
  {
    Render_Rows();
  }
  else if ((code == LV_EVENT_SCROLL_END) && !programmatic_scroll)
  {
    int32_t scroll_y = lv_obj_get_scroll_y(table_body);
    if (scroll_y < 0)
    {
      scroll_y = 0;
    }
    scroll_y = ((scroll_y + ((int32_t)UI_ROW_HEIGHT / 2)) / (int32_t)UI_ROW_HEIGHT) *
               (int32_t)UI_ROW_HEIGHT;
    programmatic_scroll = true;
    lv_obj_scroll_to_y(table_body, scroll_y, LV_ANIM_OFF);
    programmatic_scroll = false;
    Render_Rows();
  }
}

static void Clear_ScannerRow(UiScannerRowText *row)
{
  row->node_id[0] = '\0';
  row->bitrate[0] = '\0';
  row->response[0] = '\0';
  row->hint[0] = '\0';
}

static void Format_ScannerRow(UiScannerRowText *row,
                              const CanScannerResult *result)
{
  const char *hint = "";
  (void)snprintf(row->node_id, sizeof(row->node_id), "%u",
                 (unsigned int)result->node_id);
  (void)snprintf(row->bitrate, sizeof(row->bitrate), "%lu",
                 (unsigned long)result->bitrate_kbps);
  if (result->kind == CAN_SCANNER_RESPONSE_DEVICE_TYPE)
  {
    (void)snprintf(row->response, sizeof(row->response), "0x%08lX",
                   (unsigned long)result->value);
    hint = CanScanner_DeviceHint(result->value);
  }
  else if (result->kind == CAN_SCANNER_RESPONSE_ABORT)
  {
    (void)snprintf(row->response, sizeof(row->response), "AB 0x%08lX",
                   (unsigned long)result->abort_code);
  }
  else
  {
    (void)snprintf(row->response, sizeof(row->response),
                   "CS %02X  %02X%02X%02X%02X",
                   (unsigned int)result->command,
                   (unsigned int)result->data[4],
                   (unsigned int)result->data[5],
                   (unsigned int)result->data[6],
                   (unsigned int)result->data[7]);
  }
  (void)snprintf(row->hint, sizeof(row->hint), "%s", hint);
}

static void Render_ScannerRows(void)
{
  int32_t scroll_y = lv_obj_get_scroll_y(scanner_table_body);
  uint32_t first;
  uint32_t row;
  if (scroll_y < 0) scroll_y = 0;
  first = (uint32_t)scroll_y / UI_SCANNER_ROW_HEIGHT;
  for (row = 0U; row < UI_SCANNER_VISIBLE_ROWS; ++row)
  {
    uint32_t index = first + row;
    uint32_t column;
    int32_t y = (int32_t)(index * UI_SCANNER_ROW_HEIGHT) + 4;
    if (index < scanner_snapshot_count)
      Format_ScannerRow(&scanner_row_text[row], &scanner_snapshot[index]);
    else
      Clear_ScannerRow(&scanner_row_text[row]);
    for (column = 0U; column < 4U; ++column)
    {
      lv_obj_set_y(scanner_cell_labels[row][column], y);
      lv_obj_invalidate(scanner_cell_labels[row][column]);
    }
  }
}

static void Refresh_Scanner(void)
{
  CanScannerStatus scanner_status;
  uint32_t content_height;
  char text[112];
  CanScanner_GetStatus(&scanner_status);
  scanner_snapshot_count = (uint32_t)CanScanner_CopyResults(
    scanner_snapshot, CAN_SCANNER_RESULT_MAX);
  content_height = scanner_snapshot_count * UI_SCANNER_ROW_HEIGHT;
  if (content_height < UI_SCANNER_TABLE_HEIGHT)
    content_height = UI_SCANNER_TABLE_HEIGHT;
  lv_obj_set_height(scanner_spacer, (int32_t)content_height);
  Render_ScannerRows();
  lv_label_set_text(scanner_action_label, scanner_status.running ? "STOP" : "START");
  if (scanner_status.running)
  {
    (void)snprintf(text, sizeof(text),
                   "%s: %lu kbit/s, node %u  %lu/%lu  found:%lu",
                   CanScanner_PhaseName(scanner_status.phase),
                   (unsigned long)scanner_status.current_bitrate_kbps,
                   (unsigned int)scanner_status.current_node_id,
                   (unsigned long)scanner_status.completed_requests,
                   (unsigned long)scanner_status.total_requests,
                   (unsigned long)scanner_status.result_count);
  }
  else
  {
    (void)snprintf(text, sizeof(text), "%s  found:%lu",
                   CanScanner_PhaseName(scanner_status.phase),
                   (unsigned long)scanner_status.result_count);
  }
  lv_label_set_text(scanner_status_label, text);
}

static void Scanner_ActionClicked(lv_event_t *event)
{
  CanScannerStatus scanner_status;
  (void)event;
  CanScanner_GetStatus(&scanner_status);
  if (scanner_status.running)
  {
    CanScanner_Stop();
  }
  else if (!CanScanner_Start())
  {
    lv_label_set_text(scanner_status_label, "Cannot take exclusive CAN TX control");
  }
  scanner_programmatic_scroll = true;
  lv_obj_scroll_to_y(scanner_table_body, 0, LV_ANIM_OFF);
  scanner_programmatic_scroll = false;
  Refresh_Scanner();
}

static void Scanner_TableScrolled(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_SCROLL)
  {
    Render_ScannerRows();
  }
  else if ((code == LV_EVENT_SCROLL_END) && !scanner_programmatic_scroll)
  {
    int32_t scroll_y = lv_obj_get_scroll_y(scanner_table_body);
    if (scroll_y < 0) scroll_y = 0;
    scroll_y = ((scroll_y + ((int32_t)UI_SCANNER_ROW_HEIGHT / 2)) /
                (int32_t)UI_SCANNER_ROW_HEIGHT) * (int32_t)UI_SCANNER_ROW_HEIGHT;
    scanner_programmatic_scroll = true;
    lv_obj_scroll_to_y(scanner_table_body, scroll_y, LV_ANIM_OFF);
    scanner_programmatic_scroll = false;
    Render_ScannerRows();
  }
}

static void Scanner_ScreenLoaded(lv_event_t *event)
{
  (void)event;
  Refresh_Scanner();
}

static int32_t Compare_Nodes(const CanopenNodeSnapshot *left,
                             const CanopenNodeSnapshot *right)
{
  int32_t result = 0;

  switch (node_sort_key)
  {
    case NODE_SORT_LAST_SEEN:
      result = (left->last_seen_us > right->last_seen_us) ? 1 :
               ((left->last_seen_us < right->last_seen_us) ? -1 : 0);
      break;
    case NODE_SORT_FRAMES:
      result = (left->frame_count > right->frame_count) ? 1 :
               ((left->frame_count < right->frame_count) ? -1 : 0);
      break;
    case NODE_SORT_TRAFFIC:
      result = (left->frame_count > right->frame_count) ? 1 :
               ((left->frame_count < right->frame_count) ? -1 : 0);
      break;
    case NODE_SORT_RESPONSE:
      if (deep_sdo_mode)
      {
        uint32_t left_finished = left->sdo_completed_count + left->sdo_aborted_count +
                                 left->sdo_timeout_count + left->sdo_protocol_error_count;
        uint32_t right_finished = right->sdo_completed_count + right->sdo_aborted_count +
                                  right->sdo_timeout_count + right->sdo_protocol_error_count;
        uint64_t left_ratio = (uint64_t)left->sdo_completed_count * right_finished;
        uint64_t right_ratio = (uint64_t)right->sdo_completed_count * left_finished;
        if ((left_finished == 0U) && (right_finished == 0U)) result = 0;
        else if (left_finished == 0U) result = -1;
        else if (right_finished == 0U) result = 1;
        else result = (left_ratio > right_ratio) ? 1 : ((left_ratio < right_ratio) ? -1 : 0);
      }
      else if ((left->sdo_request_count == 0U) && (right->sdo_request_count == 0U))
      {
        result = 0;
      }
      else if (left->sdo_request_count == 0U)
      {
        result = -1;
      }
      else if (right->sdo_request_count == 0U)
      {
        result = 1;
      }
      else
      {
        uint32_t left_responses = (left->sdo_response_count > left->sdo_request_count) ?
                                  left->sdo_request_count : left->sdo_response_count;
        uint32_t right_responses = (right->sdo_response_count > right->sdo_request_count) ?
                                   right->sdo_request_count : right->sdo_response_count;
        uint64_t left_ratio = (uint64_t)left_responses * right->sdo_request_count;
        uint64_t right_ratio = (uint64_t)right_responses * left->sdo_request_count;
        result = (left_ratio > right_ratio) ? 1 : ((left_ratio < right_ratio) ? -1 : 0);
      }
      break;
    case NODE_SORT_TYPE:
      result = ((uint32_t)left->last_type > (uint32_t)right->last_type) ? 1 :
               (((uint32_t)left->last_type < (uint32_t)right->last_type) ? -1 : 0);
      break;
    case NODE_SORT_ID:
    default:
      result = (left->node_id > right->node_id) ? 1 :
               ((left->node_id < right->node_id) ? -1 : 0);
      break;
  }

  if ((result == 0) && (left->node_id != right->node_id))
  {
    result = (left->node_id > right->node_id) ? 1 : -1;
  }
  return node_sort_ascending ? result : -result;
}

static void Sort_Nodes(void)
{
  uint32_t i;
  for (i = 1U; i < node_snapshot_count; ++i)
  {
    CanopenNodeSnapshot value = node_snapshot[i];
    uint32_t position = i;
    while ((position > 0U) && (Compare_Nodes(&value, &node_snapshot[position - 1U]) < 0))
    {
      node_snapshot[position] = node_snapshot[position - 1U];
      --position;
    }
    node_snapshot[position] = value;
  }
}

static void Format_NodeRow(UiNodeRowText *text, const CanopenNodeSnapshot *node)
{
  uint32_t traffic_permille = 0U;
  uint32_t response_permille;

  if (analyzer_snapshot.frame_count != 0U)
  {
    traffic_permille = (uint32_t)(((uint64_t)node->frame_count * 1000U) /
                                  analyzer_snapshot.frame_count);
  }
  (void)snprintf(text->node_id, sizeof(text->node_id), "%c%u",
                 CanLogger_IsNodeSelected(node->node_id) ? '*' : ' ', node->node_id);
  (void)snprintf(text->last_seen, sizeof(text->last_seen), "%5lu.%05lu",
                 (unsigned long)(node->last_seen_us / 1000000U),
                 (unsigned long)((node->last_seen_us / 10U) % 100000U));
  (void)snprintf(text->frames, sizeof(text->frames), "%lu", (unsigned long)node->frame_count);
  (void)snprintf(text->traffic, sizeof(text->traffic), "%lu.%lu%%",
                 (unsigned long)(traffic_permille / 10U),
                 (unsigned long)(traffic_permille % 10U));
  if (deep_sdo_mode)
  {
    uint32_t finished = node->sdo_completed_count + node->sdo_aborted_count +
                        node->sdo_timeout_count + node->sdo_protocol_error_count;
    bool uncertain = (node->sdo_conflict_count != 0U) || (node->sdo_orphan_count != 0U);
    if (finished == 0U)
    {
      (void)snprintf(text->response, sizeof(text->response), uncertain ? "AMB!" : "--");
    }
    else
    {
      response_permille = (uint32_t)(((uint64_t)node->sdo_completed_count * 1000U) / finished);
      (void)snprintf(text->response, sizeof(text->response), uncertain ? "%lu.%lu%%!" : "%lu.%lu%%",
                     (unsigned long)(response_permille / 10U),
                     (unsigned long)(response_permille % 10U));
    }
  }
  else if (node->sdo_request_count == 0U)
  {
    (void)snprintf(text->response, sizeof(text->response), "--");
  }
  else
  {
    response_permille = (uint32_t)(((uint64_t)node->sdo_response_count * 1000U) /
                                   node->sdo_request_count);
    if (response_permille > 1000U)
    {
      response_permille = 1000U;
    }
    (void)snprintf(text->response, sizeof(text->response), "%lu.%lu%%",
                   (unsigned long)(response_permille / 10U),
                   (unsigned long)(response_permille % 10U));
  }
  (void)snprintf(text->type, sizeof(text->type), "%s",
                 CanopenDecoder_TypeName(node->last_type));
}

static void Clear_NodeRow(UiNodeRowText *text)
{
  text->node_id[0] = '\0';
  text->last_seen[0] = '\0';
  text->frames[0] = '\0';
  text->traffic[0] = '\0';
  text->response[0] = '\0';
  text->type[0] = '\0';
}

static void Render_NodeRows(void)
{
  int32_t scroll_y = lv_obj_get_scroll_y(nodes_table_body);
  uint32_t first;
  uint32_t row;

  if (scroll_y < 0)
  {
    scroll_y = 0;
  }
  first = (uint32_t)scroll_y / UI_NODE_ROW_HEIGHT;

  for (row = 0U; row < UI_NODE_VISIBLE_ROWS; ++row)
  {
    uint32_t index = first + row;
    int32_t y = (int32_t)(index * UI_NODE_ROW_HEIGHT) + 4;
    uint32_t column;

    if (index < node_snapshot_count)
    {
      Format_NodeRow(&node_row_text[row], &node_snapshot[index]);
      lv_obj_set_hidden(node_open_buttons[row], false);
    }
    else
    {
      Clear_NodeRow(&node_row_text[row]);
      lv_obj_set_hidden(node_open_buttons[row], true);
    }

    for (column = 0U; column < 6U; ++column)
    {
      lv_obj_set_y(node_cell_labels[row][column], y);
      lv_obj_invalidate(node_cell_labels[row][column]);
    }
    lv_obj_set_y(node_open_buttons[row], y - 3);
  }
}

static void Log_NodeToggleClicked(lv_event_t *event)
{
  uint32_t row = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
  int32_t scroll_y = lv_obj_get_scroll_y(nodes_table_body);
  uint32_t index;
  uint8_t node_id;

  if (scroll_y < 0)
  {
    scroll_y = 0;
  }
  index = ((uint32_t)scroll_y / UI_NODE_ROW_HEIGHT) + row;
  if (index >= node_snapshot_count)
  {
    return;
  }
  node_id = node_snapshot[index].node_id;
  CanLogger_SetNodeSelected(node_id, !CanLogger_IsNodeSelected(node_id));
  Render_NodeRows();
}

static void Refresh_NodesSnapshot(void)
{
  uint32_t content_height;
  char title[48];

  node_snapshot_count = CanopenAnalyzer_GetNodesSnapshot(node_snapshot,
                                                          CANOPEN_NODE_COUNT_MAX,
                                                          &analyzer_snapshot);
  Sort_Nodes();
  content_height = node_snapshot_count * UI_NODE_ROW_HEIGHT;
  if (content_height < UI_NODE_TABLE_HEIGHT)
  {
    content_height = UI_NODE_TABLE_HEIGHT;
  }
  lv_obj_set_height(nodes_spacer, (int32_t)content_height);
  (void)snprintf(title, sizeof(title), deep_sdo_mode ?
                 "Nodes DEEP %5lu.%05lu s  Unique:%lu" :
                 "Nodes  %5lu.%05lu s  Unique:%lu",
                 (unsigned long)(analyzer_snapshot.last_timestamp_us / 1000000U),
                 (unsigned long)((analyzer_snapshot.last_timestamp_us / 10U) % 100000U),
                 (unsigned long)analyzer_snapshot.unique_node_count);
  lv_label_set_text(nodes_title_label, title);
  Render_NodeRows();
}

static void Node_HeaderClicked(lv_event_t *event)
{
  NodeSortKey key = (NodeSortKey)(uintptr_t)lv_event_get_user_data(event);
  if (node_sort_key == key)
  {
    node_sort_ascending = !node_sort_ascending;
  }
  else
  {
    node_sort_key = key;
    node_sort_ascending = true;
  }
  Sort_Nodes();
  Render_NodeRows();
}

static void Nodes_TableScrolled(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_SCROLL)
  {
    Render_NodeRows();
  }
  else if ((code == LV_EVENT_SCROLL_END) && !nodes_programmatic_scroll)
  {
    int32_t scroll_y = lv_obj_get_scroll_y(nodes_table_body);
    if (scroll_y < 0)
    {
      scroll_y = 0;
    }
    scroll_y = ((scroll_y + ((int32_t)UI_NODE_ROW_HEIGHT / 2)) /
                (int32_t)UI_NODE_ROW_HEIGHT) * (int32_t)UI_NODE_ROW_HEIGHT;
    nodes_programmatic_scroll = true;
    lv_obj_scroll_to_y(nodes_table_body, scroll_y, LV_ANIM_OFF);
    nodes_programmatic_scroll = false;
    Render_NodeRows();
  }
}

static int32_t Compare_Events(const CanopenNodeEventSnapshot *left,
                              const CanopenNodeEventSnapshot *right)
{
  int32_t result = 0;
  uint32_t i;
  CanopenSdoAddress left_address = CanopenDecoder_DecodeSdoAddress(&left->frame, left->type);
  CanopenSdoAddress right_address = CanopenDecoder_DecodeSdoAddress(&right->frame, right->type);

  if (left->has_sdo_address)
  {
    left_address.index = left->object_index;
    left_address.subindex = left->object_subindex;
    left_address.data_offset = left->data_offset;
    left_address.valid = true;
  }
  if (right->has_sdo_address)
  {
    right_address.index = right->object_index;
    right_address.subindex = right->object_subindex;
    right_address.data_offset = right->data_offset;
    right_address.valid = true;
  }

  if (node_object_mode)
  {
    if (left_address.index != right_address.index)
    {
      return (left_address.index < right_address.index) ? -1 : 1;
    }
    if (left_address.subindex != right_address.subindex)
    {
      return (left_address.subindex < right_address.subindex) ? -1 : 1;
    }
    if (left->type != right->type)
    {
      if (left->type == CANOPEN_MESSAGE_SDO_RX)
      {
        return -1;
      }
      if (right->type == CANOPEN_MESSAGE_SDO_RX)
      {
        return 1;
      }
    }
    return 0;
  }

  switch (event_sort_key)
  {
    case EVENT_SORT_LATENCY:
      if (left->has_response_time != right->has_response_time)
      {
        result = left->has_response_time ? 1 : -1;
      }
      else if (left->has_response_time)
      {
        result = (left->response_time_us > right->response_time_us) ? 1 :
                 ((left->response_time_us < right->response_time_us) ? -1 : 0);
      }
      break;
    case EVENT_SORT_ID:
      result = (left->frame.id > right->frame.id) ? 1 :
               ((left->frame.id < right->frame.id) ? -1 : 0);
      break;
    case EVENT_SORT_TYPE:
      result = ((uint32_t)left->type > (uint32_t)right->type) ? 1 :
               (((uint32_t)left->type < (uint32_t)right->type) ? -1 : 0);
      break;
    case EVENT_SORT_OBJECT:
      if (left_address.valid != right_address.valid)
      {
        result = left_address.valid ? 1 : -1;
      }
      else if (left_address.valid)
      {
        result = (left_address.index > right_address.index) ? 1 :
                 ((left_address.index < right_address.index) ? -1 : 0);
      }
      break;
    case EVENT_SORT_SUBINDEX:
      if (left_address.valid != right_address.valid)
      {
        result = left_address.valid ? 1 : -1;
      }
      else if (left_address.valid)
      {
        result = (left_address.subindex > right_address.subindex) ? 1 :
                 ((left_address.subindex < right_address.subindex) ? -1 : 0);
      }
      break;
    case EVENT_SORT_DATA:
      for (i = 0U; (i < 8U) && (result == 0); ++i)
      {
        result = (left->frame.data[i] > right->frame.data[i]) ? 1 :
                 ((left->frame.data[i] < right->frame.data[i]) ? -1 : 0);
      }
      break;
    case EVENT_SORT_TIME:
    default:
      result = (left->frame.timestamp_us > right->frame.timestamp_us) ? 1 :
               ((left->frame.timestamp_us < right->frame.timestamp_us) ? -1 : 0);
      break;
  }
  return event_sort_ascending ? result : -result;
}

static void Sort_Events(void)
{
  uint32_t i;
  for (i = 1U; i < event_snapshot_count; ++i)
  {
    CanopenNodeEventSnapshot value = event_snapshot[i];
    uint32_t position = i;
    while ((position > 0U) && (Compare_Events(&value, &event_snapshot[position - 1U]) < 0))
    {
      event_snapshot[position] = event_snapshot[position - 1U];
      --position;
    }
    event_snapshot[position] = value;
  }
}

static const char *Sdo_StatusName(CanopenSdoStatus status)
{
  switch (status)
  {
    case CANOPEN_SDO_STATUS_WAITING: return "WAIT";
    case CANOPEN_SDO_STATUS_COMPLETED: return "OK";
    case CANOPEN_SDO_STATUS_ABORTED: return "ABORT";
    case CANOPEN_SDO_STATUS_TIMEOUT: return "TIME";
    case CANOPEN_SDO_STATUS_AMBIGUOUS: return "AMB";
    case CANOPEN_SDO_STATUS_PROTOCOL_ERROR: return "ERR";
    case CANOPEN_SDO_STATUS_ORPHAN: return "ORPH";
    case CANOPEN_SDO_STATUS_NONE:
    default: return "--";
  }
}

static const char *Sdo_AbortShortName(uint32_t code)
{
  switch (code)
  {
    case 0x05040000U: return "TIMEOUT";
    case 0x05040001U: return "BAD CMD";
    case 0x05040004U: return "CRC";
    case 0x05040005U: return "NO MEM";
    case 0x06010000U: return "NO ACCESS";
    case 0x06010001U: return "READ ONLY";
    case 0x06010002U: return "WRITE ONLY";
    case 0x06020000U: return "NO OBJECT";
    case 0x06060000U: return "HARDWARE";
    case 0x06070010U: return "BAD SIZE";
    case 0x06090011U: return "NO SUB";
    case 0x06090030U: return "BAD VALUE";
    case 0x06090031U: return "TOO HIGH";
    case 0x06090032U: return "TOO LOW";
    case 0x08000021U: return "LOCAL CTRL";
    case 0x08000022U: return "BAD STATE";
    default: return "SDO ABORT";
  }
}

static uint32_t Sdo_LittleEndianValue(const uint8_t *data, uint32_t size)
{
  uint32_t value = 0U;
  uint32_t index;
  for (index = 0U; (index < size) && (index < 4U); ++index)
  {
    value |= (uint32_t)data[index] << (index * 8U);
  }
  return value;
}

static void Format_EventRow(UiEventRowText *text, const CanopenNodeEventSnapshot *event)
{
  uint32_t i;
  uint32_t offset = 0U;
  uint32_t data_start = 0U;
  CanopenSdoAddress address = CanopenDecoder_DecodeSdoAddress(&event->frame, event->type);

  if (event->has_sdo_address)
  {
    address.index = event->object_index;
    address.subindex = event->object_subindex;
    address.data_offset = event->data_offset;
    address.valid = true;
  }

  (void)snprintf(text->time, sizeof(text->time), "%5lu.%05lu",
                 (unsigned long)(event->frame.timestamp_us / 1000000U),
                 (unsigned long)((event->frame.timestamp_us / 10U) % 100000U));
  text->latency[0] = '\0';
  if (event->has_response_time)
  {
    (void)snprintf(text->latency, sizeof(text->latency), "%lu",
                   (unsigned long)event->response_time_us);
  }
  (void)snprintf(text->id, sizeof(text->id), "%03lX", (unsigned long)event->frame.id);
  (void)snprintf(text->type, sizeof(text->type), "%s",
                 (deep_sdo_mode && node_object_mode) ? Sdo_StatusName(event->sdo_status) :
                                                       CanopenDecoder_TypeName(event->type));
  text->object[0] = '\0';
  text->subindex[0] = '\0';
  if (address.valid)
  {
    (void)snprintf(text->object, sizeof(text->object), "%04X", address.index);
    (void)snprintf(text->subindex, sizeof(text->subindex), "%02X", address.subindex);
    data_start = address.data_offset;
  }
  text->data[0] = '\0';
  if ((event->frame.flags & CAN_FRAME_FLAG_REMOTE) != 0U)
  {
    (void)snprintf(text->data, sizeof(text->data), "RTR");
  }
  else
  {
    uint8_t command = event->frame.data[0];
    if (address.valid && (event->abort_code != 0U || command == 0x80U))
    {
      uint32_t abort_code = event->abort_code;
      if ((abort_code == 0U) && (event->frame.dlc >= 8U))
      {
        abort_code = Sdo_LittleEndianValue(&event->frame.data[4], 4U);
      }
      (void)snprintf(text->data, sizeof(text->data), "AB %08lX %s",
                     (unsigned long)abort_code, Sdo_AbortShortName(abort_code));
      return;
    }
    if (address.valid && (event->type == CANOPEN_MESSAGE_SDO_RX) && (command == 0x40U))
    {
      (void)snprintf(text->data, sizeof(text->data), "READ REQUEST");
      return;
    }
    if (address.valid && (event->type == CANOPEN_MESSAGE_SDO_TX) && (command == 0x60U))
    {
      (void)snprintf(text->data, sizeof(text->data), "WRITE OK");
      return;
    }
    if (address.valid && (event->frame.dlc >= 8U) &&
        ((((event->type == CANOPEN_MESSAGE_SDO_RX) && ((command & 0xE0U) == 0x20U)) ||
          ((event->type == CANOPEN_MESSAGE_SDO_TX) && ((command & 0xE0U) == 0x40U))) &&
         ((command & 0x02U) != 0U)))
    {
      uint32_t size = ((command & 0x01U) != 0U) ? 4U - ((command >> 2U) & 0x03U) : 4U;
      uint32_t value = Sdo_LittleEndianValue(&event->frame.data[4], size);
      (void)snprintf(text->data, sizeof(text->data), "%s0x%0*lX (%lu)",
                     event->type == CANOPEN_MESSAGE_SDO_RX ? "WR " : "",
                     (int)(size * 2U), (unsigned long)value, (unsigned long)value);
      return;
    }
    for (i = data_start; (i < event->frame.dlc) && (offset < (sizeof(text->data) - 3U)); ++i)
    {
      offset += (uint32_t)snprintf(&text->data[offset], sizeof(text->data) - offset,
                                   (i == data_start) ? "%02X" : " %02X", event->frame.data[i]);
    }
  }
}

static void Clear_EventRow(UiEventRowText *text)
{
  text->time[0] = '\0';
  text->latency[0] = '\0';
  text->id[0] = '\0';
  text->type[0] = '\0';
  text->object[0] = '\0';
  text->subindex[0] = '\0';
  text->data[0] = '\0';
}

static void Render_EventRows(void)
{
  static const uint32_t normal_colors[7] =
  {
    0xE8EEF2U, 0xA9C7D7U, 0xE8EEF2U, 0xA9C7D7U,
    0xE8EEF2U, 0xE8EEF2U, 0xE8EEF2U
  };
  int32_t scroll_y = lv_obj_get_scroll_y(node_events_table_body);
  uint32_t first;
  uint32_t row;

  if (scroll_y < 0)
  {
    scroll_y = 0;
  }
  first = (uint32_t)scroll_y / UI_EVENT_ROW_HEIGHT;
  for (row = 0U; row < UI_EVENT_VISIBLE_ROWS; ++row)
  {
    uint32_t index = first + row;
    int32_t y = (int32_t)(index * UI_EVENT_ROW_HEIGHT) + 3;
    uint32_t column;
    if (index < event_snapshot_count)
    {
      Format_EventRow(&event_row_text[row], &event_snapshot[index]);
    }
    else
    {
      Clear_EventRow(&event_row_text[row]);
    }
    for (column = 0U; column < 7U; ++column)
    {
      bool local_transaction = (index < event_snapshot_count) &&
        event_snapshot[index].local_transaction;
      lv_obj_set_y(event_cell_labels[row][column], y);
      lv_obj_set_style_text_color(event_cell_labels[row][column],
                                  lv_color_hex(local_transaction ? 0xC084FCU :
                                               normal_colors[column]), 0);
      lv_obj_invalidate(event_cell_labels[row][column]);
    }
  }
}

static void Format_NodeEventsTitle(char *title, uint32_t capacity)
{
  uint32_t index;

  if (!deep_sdo_mode)
  {
    CanopenAnalyzer_GetGlobalSnapshot(&analyzer_snapshot);
    (void)snprintf(title, capacity, "Node %u  %5lu.%05lu s  #%lu", selected_node_id,
                   (unsigned long)(analyzer_snapshot.last_timestamp_us / 1000000U),
                   (unsigned long)((analyzer_snapshot.last_timestamp_us / 10U) % 100000U),
                   (unsigned long)event_snapshot_count);
    return;
  }

  node_snapshot_count = CanopenAnalyzer_GetNodesSnapshot(node_snapshot,
                                                          CANOPEN_NODE_COUNT_MAX,
                                                          &analyzer_snapshot);
  for (index = 0U; index < node_snapshot_count; ++index)
  {
    if (node_snapshot[index].node_id == selected_node_id)
    {
      (void)snprintf(title, capacity, "N%u %lu.%05lu OK:%lu TO:%lu AB:%lu AM:%lu",
                     selected_node_id,
                     (unsigned long)(analyzer_snapshot.last_timestamp_us / 1000000U),
                     (unsigned long)((analyzer_snapshot.last_timestamp_us / 10U) % 100000U),
                     (unsigned long)node_snapshot[index].sdo_completed_count,
                     (unsigned long)node_snapshot[index].sdo_timeout_count,
                     (unsigned long)node_snapshot[index].sdo_aborted_count,
                     (unsigned long)node_snapshot[index].sdo_conflict_count);
      return;
    }
  }
  (void)snprintf(title, capacity, "N%u %lu.%05lu OK:0 TO:0 AB:0 AM:0", selected_node_id,
                 (unsigned long)(analyzer_snapshot.last_timestamp_us / 1000000U),
                 (unsigned long)((analyzer_snapshot.last_timestamp_us / 10U) % 100000U));
}

static void Refresh_NodeEventsSnapshot(void)
{
  uint32_t content_height;
  char title[48];

  CanopenAnalyzer_GetGlobalSnapshot(&analyzer_snapshot);
  if (node_object_mode)
  {
    event_snapshot_count = CanopenAnalyzer_GetNodeObjectsSnapshot(selected_node_id,
                                                                   event_snapshot,
                                                                   CANOPEN_EVENT_SNAPSHOT_MAX);
  }
  else
  {
    event_snapshot_count = CanopenAnalyzer_GetNodeEventsSnapshot(selected_node_id,
                                                                  event_snapshot,
                                                                  CANOPEN_EVENT_SNAPSHOT_MAX);
  }
  Sort_Events();
  content_height = event_snapshot_count * UI_EVENT_ROW_HEIGHT;
  if (content_height < UI_EVENT_TABLE_HEIGHT)
  {
    content_height = UI_EVENT_TABLE_HEIGHT;
  }
  lv_obj_set_height(node_events_spacer, (int32_t)content_height);
  Format_NodeEventsTitle(title, sizeof(title));
  lv_label_set_text(node_events_title_label, title);
  Render_EventRows();
}

static void Update_NodeEventsHeader(void)
{
  char title[48];
  Format_NodeEventsTitle(title, sizeof(title));
  lv_label_set_text(node_events_title_label, title);
}

static void Set_NodeEventsPaused(bool paused)
{
  node_events_paused = paused;
  lv_label_set_text(node_events_pause_label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void NodeEvents_PauseClicked(lv_event_t *event)
{
  (void)event;
  if (node_events_paused)
  {
    events_programmatic_scroll = true;
    lv_obj_scroll_to_y(node_events_table_body, 0, LV_ANIM_OFF);
    events_programmatic_scroll = false;
    Set_NodeEventsPaused(false);
    Refresh_NodeEventsSnapshot();
  }
  else
  {
    Set_NodeEventsPaused(true);
  }
}

static void NodeEvents_ModeClicked(lv_event_t *event)
{
  (void)event;
  node_object_mode = !node_object_mode;
  lv_label_set_text(node_events_mode_label, node_object_mode ? "OBJ" : "ALL");
  lv_label_set_text(node_events_type_header_label,
                    (deep_sdo_mode && node_object_mode) ? "State" : "Type");
  event_sort_key = node_object_mode ? EVENT_SORT_OBJECT : EVENT_SORT_TIME;
  event_sort_ascending = node_object_mode;
  events_programmatic_scroll = true;
  lv_obj_scroll_to_y(node_events_table_body, 0, LV_ANIM_OFF);
  events_programmatic_scroll = false;
  Refresh_NodeEventsSnapshot();
}

static void Open_NodeEvents(lv_event_t *event)
{
  uint32_t row = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
  int32_t scroll_y = lv_obj_get_scroll_y(nodes_table_body);
  uint32_t first;
  uint32_t index;

  if (scroll_y < 0)
  {
    scroll_y = 0;
  }
  first = (uint32_t)scroll_y / UI_NODE_ROW_HEIGHT;
  index = first + row;
  if (index >= node_snapshot_count)
  {
    return;
  }

  selected_node_id = node_snapshot[index].node_id;
  node_object_mode = false;
  event_sort_key = EVENT_SORT_TIME;
  event_sort_ascending = false;
  lv_label_set_text(node_events_mode_label, "ALL");
  lv_label_set_text(node_events_type_header_label, "Type");
  Set_NodeEventsPaused(false);
  events_programmatic_scroll = true;
  lv_obj_scroll_to_y(node_events_table_body, 0, LV_ANIM_OFF);
  events_programmatic_scroll = false;
  Refresh_NodeEventsSnapshot();
  lv_screen_load(node_events_screen);
}

static void Event_HeaderClicked(lv_event_t *event)
{
  EventSortKey key = (EventSortKey)(uintptr_t)lv_event_get_user_data(event);
  if (event_sort_key == key)
  {
    event_sort_ascending = !event_sort_ascending;
  }
  else
  {
    event_sort_key = key;
    event_sort_ascending = true;
  }
  Sort_Events();
  Render_EventRows();
}

static void Events_TableScrolled(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if ((code == LV_EVENT_SCROLL_BEGIN) && !events_programmatic_scroll && !node_object_mode)
  {
    Set_NodeEventsPaused(true);
  }
  else if (code == LV_EVENT_SCROLL)
  {
    Render_EventRows();
  }
  else if ((code == LV_EVENT_SCROLL_END) && !events_programmatic_scroll)
  {
    int32_t scroll_y = lv_obj_get_scroll_y(node_events_table_body);
    if (scroll_y < 0)
    {
      scroll_y = 0;
    }
    scroll_y = ((scroll_y + ((int32_t)UI_EVENT_ROW_HEIGHT / 2)) /
                (int32_t)UI_EVENT_ROW_HEIGHT) * (int32_t)UI_EVENT_ROW_HEIGHT;
    events_programmatic_scroll = true;
    lv_obj_scroll_to_y(node_events_table_body, scroll_y, LV_ANIM_OFF);
    events_programmatic_scroll = false;
    Render_EventRows();
  }
}

static void Refresh_LogStatus(void)
{
  CanLogStatus status;
  char text[128];
  uint64_t size_kib;
  uint64_t free_mib;
  bool logging_active;

  CanLogger_GetStatus(&status);
  logging_active = status.mode != CAN_LOG_MODE_OFF;
  if (logging_active)
  {
    quick_log_mode = status.mode;
  }
  if (quick_log_mode == CAN_LOG_MODE_SELECTED)
  {
    lv_obj_remove_state(log_full_radio, LV_STATE_CHECKED);
    lv_obj_add_state(log_nodes_radio, LV_STATE_CHECKED);
  }
  else
  {
    lv_obj_add_state(log_full_radio, LV_STATE_CHECKED);
    lv_obj_remove_state(log_nodes_radio, LV_STATE_CHECKED);
  }

  lv_label_set_text(monitor_log_button_label,
                    logging_active ? "LOG " LV_SYMBOL_STOP : "LOG " LV_SYMBOL_PLAY);
  lv_label_set_text(nodes_log_button_label,
                    logging_active ? "LOG " LV_SYMBOL_STOP : "LOG " LV_SYMBOL_PLAY);
  lv_label_set_text(main_log_button_label,
                    logging_active ? "LOG " LV_SYMBOL_STOP : "LOG " LV_SYMBOL_PLAY);
  lv_obj_set_style_bg_color(main_log_button,
                            lv_color_hex(logging_active ? 0x9E3434 : 0x17698C), 0);
  lv_obj_set_style_bg_color(monitor_log_button,
                            lv_color_hex(logging_active ? 0x9E3434 : 0x17698C), 0);
  lv_obj_set_style_bg_color(nodes_log_button,
                            lv_color_hex(logging_active ? 0x9E3434 : 0x17698C), 0);
  lv_obj_set_style_border_color(monitor_log_button,
                                lv_color_hex(logging_active ? 0xFF9B9B : 0x65C7F7), 0);
  lv_obj_set_style_border_color(nodes_log_button,
                                lv_color_hex(logging_active ? 0xFF9B9B : 0x65C7F7), 0);
  lv_obj_set_style_border_color(main_log_button,
                                lv_color_hex(logging_active ? 0xFF9B9B : 0x65C7F7), 0);

  (void)snprintf(text, sizeof(text), "Card: %s%s",
                 CanLogger_CardStateName(status.card_state),
                 status.last_error != 0 ? "  !" : "");
  lv_label_set_text(log_card_label, text);

  size_kib = status.file_size_bytes / 1024U;
  (void)snprintf(text, sizeof(text), "File: %s   %llu.%02llu MiB",
                 status.file_name[0] != '\0' ? status.file_name : "--",
                 (unsigned long long)(size_kib / 1024U),
                 (unsigned long long)(((size_kib % 1024U) * 100U) / 1024U));
  lv_label_set_text(log_file_label, text);

  free_mib = status.free_bytes / (1024U * 1024U);
  (void)snprintf(text, sizeof(text), "Free: %llu.%02llu GiB",
                 (unsigned long long)(free_mib / 1024U),
                 (unsigned long long)(((free_mib % 1024U) * 100U) / 1024U));
  lv_label_set_text(log_space_label, text);

  (void)snprintf(text, sizeof(text),
                 "Stored:%llu  Lost:%llu  Write errors:%lu  Recovery:%lu",
                 (unsigned long long)status.stored_frames,
                 (unsigned long long)status.dropped_frames,
                 (unsigned long)status.write_errors,
                 (unsigned long)status.recovery_count);
  lv_label_set_text(log_counters_label, text);

  (void)snprintf(text, sizeof(text), "Selected nodes: %lu",
                 (unsigned long)status.selected_node_count);
  lv_label_set_text(log_selection_label, text);

  (void)snprintf(text, sizeof(text), "SD:%s  %s  LOST:%llu",
                 CanLogger_CardStateName(status.card_state),
                 CanLogger_ModeName(status.mode),
                 (unsigned long long)status.dropped_frames);
  lv_label_set_text(main_storage_label, text);
}

void Ui_Init(void)
{
  DeviceSettings initial_settings;
  const UiBitrateOption *initial_bitrate;
  lv_display_t *display;
  lv_indev_t *touch;
  lv_obj_t *screen;
  lv_obj_t *header_panel;
  lv_obj_t *bitrate_selector;
  lv_obj_t *bitrate_option_button;
  lv_obj_t *bitrate_option_label;
  lv_obj_t *deep_sdo_button;
  lv_obj_t *reset_button;
  lv_obj_t *pause_button;
  lv_obj_t *mode_button;
  lv_obj_t *clear_nodes_button;
  lv_obj_t *title;
  lv_obj_t *overview;
  lv_obj_t *version;
  lv_obj_t *button;
  uint32_t option;
  uint32_t row;
  uint32_t line;

  AppSettings_Get(&initial_settings);
  initial_bitrate = Find_BitrateOption(initial_settings.can_bitrate_kbps);
  deep_sdo_mode = initial_settings.deep_sdo_enabled;

  display_last_touch_ms = HAL_GetTick();
  display_wake_touch_active = false;
  Display_SetBacklight(true);

  lv_init();
  lv_tick_set_cb(HAL_GetTick);

  display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, Display_Flush);
  lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

  touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, Touch_Read);

  main_screen = lv_screen_active();
  monitor_screen = lv_obj_create(NULL);
  settings_screen = lv_obj_create(NULL);
  network_screen = lv_obj_create(NULL);
  error_screen = lv_obj_create(NULL);
  nodes_screen = lv_obj_create(NULL);
  node_events_screen = lv_obj_create(NULL);
  info_screen = lv_obj_create(NULL);
  log_screen = lv_obj_create(NULL);
  tx_screen = lv_obj_create(NULL);
  scanner_screen = lv_obj_create(NULL);
  nodes_return_screen = main_screen;
  Style_Screen(main_screen);
  Style_Screen(monitor_screen);
  Style_Screen(settings_screen);
  Style_Screen(network_screen);
  Style_Screen(error_screen);
  lv_obj_set_scrollable(network_screen, false);
  lv_obj_set_scrollbar_mode(network_screen, LV_SCROLLBAR_MODE_OFF);
  Style_Screen(nodes_screen);
  Style_Screen(node_events_screen);
  Style_Screen(info_screen);
  Style_Screen(log_screen);
  Style_Screen(tx_screen);
  Style_Screen(scanner_screen);
  lv_obj_set_style_text_font(info_screen, &ui_font_cyrillic_14, 0);

  bridge_indicator_label = lv_label_create(lv_layer_top());
  lv_obj_set_pos(bridge_indicator_label, 52, 5);
  lv_obj_set_size(bridge_indicator_label, 134, 26);
  lv_obj_set_style_bg_color(bridge_indicator_label, lv_color_hex(0x6D28D9), 0);
  lv_obj_set_style_bg_opa(bridge_indicator_label, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(bridge_indicator_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(bridge_indicator_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(bridge_indicator_label, 5, 0);
  lv_obj_set_style_radius(bridge_indicator_label, 5, 0);
  lv_label_set_text(bridge_indicator_label, "BRIDGE ACTIVE");
  lv_obj_set_hidden(bridge_indicator_label, true);

  scanner_indicator_label = lv_label_create(lv_layer_top());
  lv_obj_set_pos(scanner_indicator_label, 52, 5);
  lv_obj_set_size(scanner_indicator_label, 134, 26);
  lv_obj_set_style_bg_color(scanner_indicator_label, lv_color_hex(0xB45309), 0);
  lv_obj_set_style_bg_opa(scanner_indicator_label, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scanner_indicator_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(scanner_indicator_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(scanner_indicator_label, 5, 0);
  lv_obj_set_style_radius(scanner_indicator_label, 5, 0);
  lv_label_set_text(scanner_indicator_label, "SCANNER ACTIVE");
  lv_obj_set_hidden(scanner_indicator_label, true);

  title = Create_Label(main_screen, 0, 14, LCD_WIDTH, "CAN Sniffer", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  main_status_label = Create_Label(main_screen, 20, 42, 440, "CAN starting...", lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(main_status_label, LV_TEXT_ALIGN_CENTER, 0);
  (void)Create_NavigationButton(main_screen, 8, 68, 148, 66,
                                "CAN Monitor", monitor_screen);
  (void)Create_NavigationButton(main_screen, 166, 68, 148, 66,
                                "Nodes", nodes_screen);
  (void)Create_NavigationButton(main_screen, 324, 68, 148, 66,
                                "CAN Scanner", scanner_screen);
  (void)Create_NavigationButton(main_screen, 8, 144, 148, 66,
                                "microSD Log", log_screen);
  tx_nav_button = Create_NavigationButton(main_screen, 166, 144, 148, 66,
                                           "CANopen SDO", tx_screen);
  (void)Create_NavigationButton(main_screen, 324, 144, 148, 66,
                                "Error Logs", error_screen);
  main_storage_label = Create_Label(main_screen, 16, 230, 448,
                                    "SD:No card  Off  LOST:0", lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(main_storage_label, LV_TEXT_ALIGN_CENTER, 0);
  reset_button = Create_NavigationButton(main_screen, 4, 3, 44, 34, LV_SYMBOL_REFRESH, main_screen);
  lv_obj_add_event_cb(reset_button, Reset_StatisticsClicked, LV_EVENT_CLICKED, NULL);
  main_log_button = Create_QuickLogButton(main_screen, 304, &main_log_button_label);
  (void)Create_NavigationButton(main_screen, 384, 3, 44, 34, "i", info_screen);
  (void)Create_NavigationButton(main_screen, 432, 3, 44, 34, LV_SYMBOL_SETTINGS, settings_screen);

  (void)Create_NavigationButton(error_screen, 4, 3, 44, 34,
                                LV_SYMBOL_HOME, main_screen);
  title = Create_Label(error_screen, 58, 10, 410, "CAN Error Logs",
                       lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  error_summary_label = Create_Label(error_screen, 16, 48, 448,
                                     "Interrupts: 0     CAN RX: 0     DROP: 0",
                                     lv_color_hex(0xE8EEF2));
  lv_obj_set_style_text_align(error_summary_label, LV_TEXT_ALIGN_CENTER, 0);
  error_protocol_label = Create_Label(error_screen, 24, 82, 210,
                                      "PROTOCOL\nACK              0\nCRC              0\n"
                                      "Stuff            0\nForm             0\n"
                                      "Bit recessive    0\nBit dominant     0",
                                      lv_color_hex(0xE8EEF2));
  lv_label_set_long_mode(error_protocol_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(error_protocol_label, 138);
  error_state_label = Create_Label(error_screen, 258, 82, 198,
                                   "CONTROLLER\nWarning          0\nPassive          0\n"
                                   "Bus-off          0\nUnclassified     0",
                                   lv_color_hex(0xE8EEF2));
  lv_label_set_long_mode(error_state_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(error_state_label, 110);
  error_last_label = Create_Label(error_screen, 16, 228, 448,
                                  "Last: none at 0.000 s    TEC:0  REC:0  ESR:0x00000000",
                                  lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(error_last_label, LV_TEXT_ALIGN_CENTER, 0);

  (void)Create_NavigationButton(scanner_screen, 4, 3, 44, 34,
                                LV_SYMBOL_HOME, main_screen);
  title = Create_Label(scanner_screen, 56, 10, 294, "CANopen node scanner",
                       lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  scanner_action_button = Create_NavigationButton(scanner_screen, 364, 3, 112, 34,
                                                   "START", NULL);
  scanner_action_label = lv_obj_get_child(scanner_action_button, 1);
  lv_obj_add_event_cb(scanner_action_button, Scanner_ActionClicked,
                      LV_EVENT_CLICKED, NULL);
  scanner_status_label = Create_Label(scanner_screen, 8, 43, 464,
                                      "ready  found:0", lv_color_hex(0xA9C7D7));
  lv_label_set_long_mode(scanner_status_label, LV_LABEL_LONG_DOT);
  lv_obj_set_height(scanner_status_label, 20);

  header_panel = lv_obj_create(scanner_screen);
  lv_obj_remove_style_all(header_panel);
  lv_obj_set_pos(header_panel, 0, 61);
  lv_obj_set_size(header_panel, LCD_WIDTH, 21);
  lv_obj_set_style_bg_color(header_panel, lv_color_hex(0x17242B), 0);
  lv_obj_set_style_bg_opa(header_panel, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(header_panel, false);
  (void)Create_Label(header_panel, 4, 3, 42, "Node", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, 50, 3, 62, "kbit/s", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, 116, 3, 122, "Response 1000:00", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, 242, 3, 234, "Registered device hint",
                     lv_color_hex(0x65C7F7));

  scanner_table_body = lv_obj_create(scanner_screen);
  lv_obj_remove_style_all(scanner_table_body);
  lv_obj_set_pos(scanner_table_body, 0, UI_SCANNER_TABLE_Y);
  lv_obj_set_size(scanner_table_body, LCD_WIDTH, UI_SCANNER_TABLE_HEIGHT);
  lv_obj_set_style_bg_color(scanner_table_body, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(scanner_table_body, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(scanner_table_body, 0, 0);
  lv_obj_set_scroll_dir(scanner_table_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scanner_table_body, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(scanner_table_body, lv_color_hex(0x65C7F7),
                            LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(scanner_table_body, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(scanner_table_body, 4, LV_PART_SCROLLBAR);
  lv_obj_add_event_cb(scanner_table_body, Scanner_TableScrolled, LV_EVENT_ALL, NULL);
  scanner_spacer = lv_obj_create(scanner_table_body);
  lv_obj_remove_style_all(scanner_spacer);
  lv_obj_set_pos(scanner_spacer, 0, 0);
  lv_obj_set_size(scanner_spacer, 1, UI_SCANNER_TABLE_HEIGHT);
  lv_obj_set_clickable(scanner_spacer, false);
  lv_obj_set_scrollable(scanner_spacer, false);
  for (row = 0U; row < UI_SCANNER_VISIBLE_ROWS; ++row)
  {
    int32_t y = (int32_t)(row * UI_SCANNER_ROW_HEIGHT) + 4;
    Clear_ScannerRow(&scanner_row_text[row]);
    scanner_cell_labels[row][0] = Create_Label(scanner_table_body, 4, y, 42, "",
                                                lv_color_hex(0xE8EEF2));
    scanner_cell_labels[row][1] = Create_Label(scanner_table_body, 50, y, 62, "",
                                                lv_color_hex(0xA9C7D7));
    scanner_cell_labels[row][2] = Create_Label(scanner_table_body, 116, y, 122, "",
                                                lv_color_hex(0xE8EEF2));
    scanner_cell_labels[row][3] = Create_Label(scanner_table_body, 242, y, 234, "",
                                                lv_color_hex(0x65C7F7));
    lv_label_set_text_static(scanner_cell_labels[row][0], scanner_row_text[row].node_id);
    lv_label_set_text_static(scanner_cell_labels[row][1], scanner_row_text[row].bitrate);
    lv_label_set_text_static(scanner_cell_labels[row][2], scanner_row_text[row].response);
    lv_label_set_text_static(scanner_cell_labels[row][3], scanner_row_text[row].hint);
  }
  Create_GridLine(scanner_screen, 0, 60, LCD_WIDTH, 1);
  Create_GridLine(scanner_screen, 0, 81, LCD_WIDTH, 1);
  Create_GridLine(scanner_screen, 47, 61, 1, 165);
  Create_GridLine(scanner_screen, 113, 61, 1, 165);
  Create_GridLine(scanner_screen, 239, 61, 1, 165);
  for (line = 1U; line < UI_SCANNER_VISIBLE_ROWS; ++line)
  {
    Create_GridLine(scanner_screen, 0,
                    UI_SCANNER_TABLE_Y + (int32_t)(line * UI_SCANNER_ROW_HEIGHT),
                    LCD_WIDTH, 1);
  }
  lv_obj_add_event_cb(scanner_screen, Scanner_ScreenLoaded,
                      LV_EVENT_SCREEN_LOADED, NULL);

  (void)Create_NavigationButton(tx_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(tx_screen, 56, 10, 416, "CANopen SDO client",
                       lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

  (void)Create_Label(tx_screen, 8, 53, 38, "Node", lv_color_hex(0xA9C7D7));
  tx_node_textarea = lv_textarea_create(tx_screen);
  lv_obj_set_pos(tx_node_textarea, 46, 41);
  lv_obj_set_size(tx_node_textarea, 54, 38);
  Tx_StyleField(tx_node_textarea);
  lv_textarea_set_one_line(tx_node_textarea, true);
  lv_textarea_set_max_length(tx_node_textarea, 3U);
  lv_textarea_set_accepted_chars(tx_node_textarea, "0123456789");
  lv_textarea_set_text(tx_node_textarea, "1");
  lv_obj_add_event_cb(tx_node_textarea, Tx_FieldClicked, LV_EVENT_CLICKED, NULL);

  button = Create_NavigationButton(tx_screen, 108, 41, 102, 38, "READ", NULL);
  tx_command_label = lv_obj_get_child(button, 1);
  lv_obj_add_event_cb(button, Tx_CommandClicked, LV_EVENT_CLICKED, NULL);

  (void)Create_Label(tx_screen, 218, 53, 46, "Object", lv_color_hex(0xA9C7D7));
  tx_index_textarea = lv_textarea_create(tx_screen);
  lv_obj_set_pos(tx_index_textarea, 266, 41);
  lv_obj_set_size(tx_index_textarea, 92, 38);
  Tx_StyleField(tx_index_textarea);
  lv_textarea_set_one_line(tx_index_textarea, true);
  lv_textarea_set_max_length(tx_index_textarea, 4U);
  lv_textarea_set_accepted_chars(tx_index_textarea, "0123456789abcdefABCDEF");
  lv_textarea_set_text(tx_index_textarea, "1018");
  lv_obj_add_event_cb(tx_index_textarea, Tx_FieldClicked, LV_EVENT_CLICKED, NULL);

  (void)Create_Label(tx_screen, 366, 53, 28, "Sub", lv_color_hex(0xA9C7D7));
  tx_subindex_textarea = lv_textarea_create(tx_screen);
  lv_obj_set_pos(tx_subindex_textarea, 396, 41);
  lv_obj_set_size(tx_subindex_textarea, 76, 38);
  Tx_StyleField(tx_subindex_textarea);
  lv_textarea_set_one_line(tx_subindex_textarea, true);
  lv_textarea_set_max_length(tx_subindex_textarea, 2U);
  lv_textarea_set_accepted_chars(tx_subindex_textarea, "0123456789abcdefABCDEF");
  lv_textarea_set_text(tx_subindex_textarea, "01");
  lv_obj_add_event_cb(tx_subindex_textarea, Tx_FieldClicked, LV_EVENT_CLICKED, NULL);

  (void)Create_Label(tx_screen, 8, 101, 46, "Value", lv_color_hex(0xA9C7D7));
  tx_value_textarea = lv_textarea_create(tx_screen);
  lv_obj_set_pos(tx_value_textarea, 58, 89);
  lv_obj_set_size(tx_value_textarea, 414, 38);
  Tx_StyleField(tx_value_textarea);
  lv_textarea_set_one_line(tx_value_textarea, true);
  lv_textarea_set_max_length(tx_value_textarea, 8U);
  lv_textarea_set_accepted_chars(tx_value_textarea, "0123456789abcdefABCDEF");
  lv_textarea_set_text(tx_value_textarea, "0");
  lv_obj_add_event_cb(tx_value_textarea, Tx_FieldClicked, LV_EVENT_CLICKED, NULL);

  tx_send_button = Create_NavigationButton(tx_screen, 8, 135, 464, 42, "SEND", NULL);
  lv_obj_add_event_cb(tx_send_button, Tx_SendClicked, LV_EVENT_CLICKED, NULL);

  (void)Create_Label(tx_screen, 8, 198, 50, "Period", lv_color_hex(0xA9C7D7));
  tx_period_textarea = lv_textarea_create(tx_screen);
  lv_obj_set_pos(tx_period_textarea, 62, 185);
  lv_obj_set_size(tx_period_textarea, 100, 38);
  Tx_StyleField(tx_period_textarea);
  lv_textarea_set_one_line(tx_period_textarea, true);
  lv_textarea_set_max_length(tx_period_textarea, 7U);
  lv_textarea_set_accepted_chars(tx_period_textarea, "0123456789");
  lv_textarea_set_text(tx_period_textarea, "1000");
  lv_obj_add_event_cb(tx_period_textarea, Tx_FieldClicked, LV_EVENT_CLICKED, NULL);
  (void)Create_Label(tx_screen, 166, 198, 28, "ms", lv_color_hex(0xA9C7D7));

  tx_mode_button = Create_NavigationButton(tx_screen, 200, 185, 272, 38,
                                            "START PERIODIC SEND", NULL);
  tx_mode_label = lv_obj_get_child(tx_mode_button, 1);
  lv_obj_add_event_cb(tx_mode_button, Tx_ModeClicked, LV_EVENT_CLICKED, NULL);

  tx_status_label = Create_Label(tx_screen, 8, 238, 464, "",
                                 lv_color_hex(0xA9C7D7));
  lv_label_set_long_mode(tx_status_label, LV_LABEL_LONG_DOT);
  lv_obj_set_height(tx_status_label, 24);

  tx_keyboard = lv_keyboard_create(tx_screen);
  lv_keyboard_set_map(tx_keyboard, LV_KEYBOARD_MODE_USER_1,
                      tx_hex_keyboard_map, tx_hex_keyboard_ctrl);
  lv_keyboard_set_mode(tx_keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_align(tx_keyboard, LV_ALIGN_TOP_LEFT, 0, 84);
  lv_obj_set_size(tx_keyboard, LCD_WIDTH, LCD_HEIGHT - 84);
  lv_obj_add_event_cb(tx_keyboard, Tx_KeyboardEvent, LV_EVENT_ALL, NULL);
  lv_obj_set_hidden(tx_keyboard, true);

  tx_edit_panel = lv_obj_create(tx_screen);
  lv_obj_remove_style_all(tx_edit_panel);
  lv_obj_set_pos(tx_edit_panel, 0, 38);
  lv_obj_set_size(tx_edit_panel, LCD_WIDTH, 45);
  lv_obj_set_style_bg_color(tx_edit_panel, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(tx_edit_panel, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(tx_edit_panel, false);
  tx_edit_label = Create_Label(tx_edit_panel, 52, 15, 56, "Value",
                               lv_color_hex(0x65C7F7));
  lv_obj_set_hidden(tx_edit_panel, true);

  lv_obj_add_event_cb(tx_screen, Tx_ScreenLoaded, LV_EVENT_SCREEN_LOADED, NULL);
  tx_command = CAN_TX_SDO_READ;
  Tx_UpdateFormLabels();

  (void)Create_NavigationButton(log_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(log_screen, 58, 10, 410, "microSD logging", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  (void)Create_Label(log_screen, 16, 64, 46, "Mode:", lv_color_hex(0xA9C7D7));
  log_full_radio = Create_LogModeRadio(log_screen, 64, "Full", CAN_LOG_MODE_FULL);
  log_nodes_radio = Create_LogModeRadio(log_screen, 146, "Nodes",
                                        CAN_LOG_MODE_SELECTED);
  log_card_label = Create_Label(log_screen, 244, 63, 220, "Card: No card",
                                lv_color_hex(0xE8EEF2));
  log_file_label = Create_Label(log_screen, 16, 108, 448, "File: --",
                                lv_color_hex(0xE8EEF2));
  log_space_label = Create_Label(log_screen, 16, 133, 448, "Free: 0.00 GiB",
                                 lv_color_hex(0xA9C7D7));
  log_counters_label = Create_Label(log_screen, 16, 158, 448,
                                    "Stored:0  Lost:0  Write errors:0  Recovery:0",
                                    lv_color_hex(0xE8EEF2));
  log_selection_label = Create_Label(log_screen, 16, 183, 220, "Selected nodes: 0",
                                     lv_color_hex(0x65C7F7));
  clear_nodes_button = Create_NavigationButton(log_screen, 16, 207, 116, 46,
                                               "Clear nodes", log_screen);
  lv_obj_add_event_cb(clear_nodes_button, Log_ClearNodesClicked, LV_EVENT_CLICKED, NULL);
  (void)Create_NavigationButton(log_screen, 142, 207, 142, 46,
                                "Select nodes", nodes_screen);
  overview = Create_Label(log_screen, 300, 204, 164,
                          "On Nodes screen tap the node number. '*' means selected.",
                          lv_color_hex(0xA9C7D7));
  lv_label_set_long_mode(overview, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(overview, 58);

  (void)Create_NavigationButton(info_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(info_screen, 58, 10, 410, "Информация", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &ui_font_cyrillic_14, 0);
  overview = Create_Label(info_screen, 16, 45, 448,
                          "Classic CAN/CANopen анализатор: после reset всегда пассивный. "
                          "Передача доступна только через отдельное подтверждение в разделе TX.",
                          lv_color_hex(0xE8EEF2));
  lv_label_set_long_mode(overview, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(overview, 48);
  lv_obj_set_style_text_align(overview, LV_TEXT_ALIGN_CENTER, 0);
  (void)Create_InfoModeButton(info_screen, 4, 90, "Монитор", info_monitor_text);
  (void)Create_InfoModeButton(info_screen, 99, 90, "Ноды", info_nodes_text);
  (void)Create_InfoModeButton(info_screen, 194, 90, "Объекты", info_objects_text);
  (void)Create_InfoModeButton(info_screen, 289, 90, "Скорость", info_bitrate_text);
  (void)Create_InfoModeButton(info_screen, 384, 92, "Пакеты", info_packet_types_text);
  info_description_label = Create_Label(info_screen, 16, 150, 448,
                                        "Нажмите кнопку режима, чтобы увидеть описание.",
                                        lv_color_hex(0xA9C7D7));
  lv_label_set_long_mode(info_description_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_height(info_description_label, 78);
  lv_obj_set_style_text_align(info_description_label, LV_TEXT_ALIGN_CENTER, 0);
  version = Create_Label(info_screen, 16, 249, 448,
                         "Версия ПО " CAN_SNIFFER_VERSION,
                         lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, 0);

  (void)Create_NavigationButton(settings_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(settings_screen, 58, 10, 410, "Settings", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  (void)Create_Label(settings_screen, 48, 82, 112, "CAN bitrate", lv_color_hex(0xE8EEF2));
  bitrate_selector = lv_button_create(settings_screen);
  lv_obj_set_pos(bitrate_selector, 174, 66);
  lv_obj_set_size(bitrate_selector, 180, 42);
  lv_obj_set_style_bg_color(bitrate_selector, lv_color_hex(0x21343E), 0);
  lv_obj_set_style_bg_opa(bitrate_selector, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(bitrate_selector, lv_color_hex(0x17698C), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(bitrate_selector, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(bitrate_selector, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_border_width(bitrate_selector, 1, 0);
  lv_obj_set_style_radius(bitrate_selector, 0, 0);
  lv_obj_set_style_pad_all(bitrate_selector, 0, 0);
  lv_obj_add_event_cb(bitrate_selector, Bitrate_SelectorClicked, LV_EVENT_CLICKED, NULL);
  bitrate_selector_label = lv_label_create(bitrate_selector);
  lv_label_set_text(bitrate_selector_label, initial_bitrate->selector_text);
  lv_obj_set_style_text_color(bitrate_selector_label, lv_color_hex(0xF4FAFD), 0);
  lv_obj_center(bitrate_selector_label);

  (void)Create_Label(settings_screen, 48, 145, 112, "SDO analysis", lv_color_hex(0xE8EEF2));
  deep_sdo_button = lv_button_create(settings_screen);
  lv_obj_set_pos(deep_sdo_button, 174, 129);
  lv_obj_set_size(deep_sdo_button, 180, 42);
  lv_obj_set_style_bg_color(deep_sdo_button, lv_color_hex(0x21343E), 0);
  lv_obj_set_style_bg_opa(deep_sdo_button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(deep_sdo_button, lv_color_hex(0x17698C), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(deep_sdo_button, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_border_width(deep_sdo_button, 1, 0);
  lv_obj_set_style_radius(deep_sdo_button, 0, 0);
  lv_obj_set_style_pad_all(deep_sdo_button, 0, 0);
  lv_obj_add_event_cb(deep_sdo_button, DeepSdo_ModeClicked, LV_EVENT_CLICKED, NULL);
  deep_sdo_button_label = lv_label_create(deep_sdo_button);
  lv_label_set_text(deep_sdo_button_label, deep_sdo_mode ? "[X] Deep" : "[ ] Basic");
  lv_obj_set_style_text_color(deep_sdo_button_label, lv_color_hex(0xF4FAFD), 0);
  lv_obj_center(deep_sdo_button_label);

  bitrate_options_panel = lv_obj_create(settings_screen);
  lv_obj_set_pos(bitrate_options_panel, 100, 108);
  lv_obj_set_size(bitrate_options_panel, 280, 158);
  lv_obj_set_scrollable(bitrate_options_panel, false);
  lv_obj_set_style_bg_color(bitrate_options_panel, lv_color_hex(0x21343E), 0);
  lv_obj_set_style_bg_opa(bitrate_options_panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(bitrate_options_panel, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_border_width(bitrate_options_panel, 1, 0);
  lv_obj_set_style_radius(bitrate_options_panel, 0, 0);
  lv_obj_set_style_pad_all(bitrate_options_panel, 0, 0);

  for (option = 0U; option < (sizeof(bitrate_options) / sizeof(bitrate_options[0])); ++option)
  {
    bitrate_option_button = lv_button_create(bitrate_options_panel);
    lv_obj_set_pos(bitrate_option_button,
                   8 + (int32_t)((option / 5U) * 134U),
                   8 + (int32_t)((option % 5U) * 29U));
    lv_obj_set_size(bitrate_option_button, 128, 26);
    lv_obj_set_style_bg_color(bitrate_option_button, lv_color_hex(0x17698C), 0);
    lv_obj_set_style_bg_opa(bitrate_option_button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bitrate_option_button, lv_color_hex(0x238DB8), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(bitrate_option_button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bitrate_option_button, lv_color_hex(0x65C7F7), 0);
    lv_obj_set_style_border_width(bitrate_option_button, 1, 0);
    lv_obj_set_style_radius(bitrate_option_button, 0, 0);
    lv_obj_set_style_pad_all(bitrate_option_button, 0, 0);
    lv_obj_add_event_cb(bitrate_option_button, Bitrate_OptionClicked, LV_EVENT_CLICKED,
                        (void *)&bitrate_options[option]);
    bitrate_option_label = lv_label_create(bitrate_option_button);
    lv_label_set_text(bitrate_option_label, bitrate_options[option].name);
    lv_obj_set_style_text_color(bitrate_option_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bitrate_option_label);
  }
  lv_obj_set_hidden(bitrate_options_panel, true);

  (void)Create_NavigationButton(settings_screen, 174, 192, 180, 46,
                                "Network settings", network_screen);
  lv_obj_add_event_cb(settings_screen, Settings_ScreenLoaded,
                      LV_EVENT_SCREEN_LOADED, NULL);

  (void)Create_NavigationButton(network_screen, 4, 3, 44, 34,
                                LV_SYMBOL_LEFT, settings_screen);
  title = Create_Label(network_screen, 58, 10, 410, "Ethernet / IPv4",
                       lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);

  network_dhcp_label = Create_Label(network_screen, 34, 49, 90, "DHCP",
                                    lv_color_hex(0xE8EEF2));
  network_dhcp_switch = lv_button_create(network_screen);
  lv_obj_set_pos(network_dhcp_switch, 126, 43);
  lv_obj_set_size(network_dhcp_switch, 92, 30);
  lv_obj_set_checkable(network_dhcp_switch, true);
  lv_obj_set_style_bg_color(network_dhcp_switch, lv_color_hex(0x21343E), 0);
  lv_obj_set_style_bg_opa(network_dhcp_switch, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(network_dhcp_switch, lv_color_hex(0x238DB8),
                            LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(network_dhcp_switch, LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(network_dhcp_switch, lv_color_hex(0x17698C),
                            LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(network_dhcp_switch, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(network_dhcp_switch, lv_color_hex(0x65C7F7), 0);
  lv_obj_set_style_border_width(network_dhcp_switch, 1, 0);
  lv_obj_set_style_radius(network_dhcp_switch, 4, 0);
  lv_obj_set_style_pad_all(network_dhcp_switch, 0, 0);
  lv_obj_add_event_cb(network_dhcp_switch, Network_DhcpChanged,
                      LV_EVENT_VALUE_CHANGED, NULL);
  network_dhcp_state_label = lv_label_create(network_dhcp_switch);
  lv_label_set_text(network_dhcp_state_label, "OFF");
  lv_obj_set_style_text_color(network_dhcp_state_label, lv_color_hex(0xF4FAFD), 0);
  lv_obj_center(network_dhcp_state_label);

  network_ip_label = Create_Label(network_screen, 34, 91, 90, "Static IP",
                                  lv_color_hex(0xE8EEF2));
  network_ip_textarea = lv_textarea_create(network_screen);
  lv_obj_set_pos(network_ip_textarea, 126, 78);
  lv_obj_set_size(network_ip_textarea, 206, 38);

  network_mask_label = Create_Label(network_screen, 34, 135, 90, "Netmask",
                                    lv_color_hex(0xE8EEF2));
  network_mask_textarea = lv_textarea_create(network_screen);
  lv_obj_set_pos(network_mask_textarea, 126, 122);
  lv_obj_set_size(network_mask_textarea, 206, 38);

  network_gateway_label = Create_Label(network_screen, 34, 179, 90, "Gateway",
                                       lv_color_hex(0xE8EEF2));
  network_gateway_textarea = lv_textarea_create(network_screen);
  lv_obj_set_pos(network_gateway_textarea, 126, 166);
  lv_obj_set_size(network_gateway_textarea, 206, 38);

  {
    lv_obj_t *fields[] =
    {
      network_ip_textarea, network_mask_textarea, network_gateway_textarea
    };
    uint32_t field;
    for (field = 0U; field < (sizeof(fields) / sizeof(fields[0])); ++field)
    {
      lv_textarea_set_one_line(fields[field], true);
      lv_textarea_set_max_length(fields[field], 15U);
      lv_textarea_set_accepted_chars(fields[field], "0123456789.");
      lv_obj_set_style_bg_color(fields[field], lv_color_hex(0x21343E), 0);
      lv_obj_set_style_text_color(fields[field], lv_color_hex(0xF4FAFD), 0);
      lv_obj_set_style_border_color(fields[field], lv_color_hex(0x65C7F7), 0);
      lv_obj_set_style_border_width(fields[field], 1, 0);
      lv_obj_set_style_radius(fields[field], 0, 0);
      lv_obj_set_style_pad_all(fields[field], 7, 0);
      lv_obj_add_event_cb(fields[field], Network_FieldClicked, LV_EVENT_CLICKED, NULL);
    }
  }

  network_save_button = Create_NavigationButton(network_screen, 350, 78, 104, 82,
                                                "Save\nand apply", NULL);
  lv_obj_add_event_cb(network_save_button, Network_SaveClicked, LV_EVENT_CLICKED, NULL);
  network_status_label = Create_Label(network_screen, 34, 228, 420,
                                      "Edit and press Save", lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(network_status_label, LV_TEXT_ALIGN_CENTER, 0);

  network_edit_label = Create_Label(network_screen, 34, 55, 90, "Static IP",
                                    lv_color_hex(0xE8EEF2));
  lv_obj_set_hidden(network_edit_label, true);

  network_keyboard = lv_keyboard_create(network_screen);
  lv_keyboard_set_mode(network_keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_size(network_keyboard, LCD_WIDTH, LCD_HEIGHT - 86);
  lv_obj_align(network_keyboard, LV_ALIGN_TOP_LEFT, 0, 86);
  lv_obj_add_event_cb(network_keyboard, Network_KeyboardEvent, LV_EVENT_ALL, NULL);
  lv_obj_set_hidden(network_keyboard, true);
  lv_obj_add_event_cb(network_screen, Network_ScreenLoaded,
                      LV_EVENT_SCREEN_LOADED, NULL);
  lv_obj_add_event_cb(network_screen, Network_ScreenUnloaded,
                      LV_EVENT_SCREEN_UNLOADED, NULL);

  nodes_back_button = Create_NavigationButton(nodes_screen, 4, 3, 44, 34,
                                               LV_SYMBOL_HOME, main_screen);
  nodes_back_button_label = lv_obj_get_child(nodes_back_button, 1);
  nodes_title_label = Create_Label(nodes_screen, 58, 10, 334, "Nodes  0.00000 s  Unique:0",
                                   lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(nodes_title_label, &lv_font_montserrat_14, 0);
  nodes_log_button = Create_QuickLogButton(nodes_screen, 400, &nodes_log_button_label);

  header_panel = lv_obj_create(nodes_screen);
  lv_obj_remove_style_all(header_panel);
  lv_obj_set_pos(header_panel, 0, 36);
  lv_obj_set_size(header_panel, LCD_WIDTH, 23);
  lv_obj_set_style_bg_color(header_panel, lv_color_hex(0x17242B), 0);
  lv_obj_set_style_bg_opa(header_panel, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(header_panel, false);
  (void)Create_TableHeader(header_panel, 2, 34, "*ID", Node_HeaderClicked, NODE_SORT_ID);
  (void)Create_TableHeader(header_panel, 40, 67, "Last", Node_HeaderClicked, NODE_SORT_LAST_SEEN);
  (void)Create_TableHeader(header_panel, 111, 57, "Frames", Node_HeaderClicked, NODE_SORT_FRAMES);
  (void)Create_TableHeader(header_panel, 172, 56, "Traffic", Node_HeaderClicked, NODE_SORT_TRAFFIC);
  nodes_response_header_label = lv_obj_get_child(
    Create_TableHeader(header_panel, 232, 56, "SDO %", Node_HeaderClicked, NODE_SORT_RESPONSE), 0);
  (void)Create_TableHeader(header_panel, 292, 142, "Last type", Node_HeaderClicked, NODE_SORT_TYPE);
  (void)Create_TableHeader(header_panel, 438, 42, ">", NULL, 0U);

  nodes_table_body = lv_obj_create(nodes_screen);
  lv_obj_remove_style_all(nodes_table_body);
  lv_obj_set_pos(nodes_table_body, 0, UI_NODE_TABLE_Y);
  lv_obj_set_size(nodes_table_body, LCD_WIDTH, UI_NODE_TABLE_HEIGHT);
  lv_obj_set_style_bg_color(nodes_table_body, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(nodes_table_body, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(nodes_table_body, 0, 0);
  lv_obj_set_scroll_dir(nodes_table_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(nodes_table_body, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(nodes_table_body, lv_color_hex(0x65C7F7), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(nodes_table_body, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(nodes_table_body, 4, LV_PART_SCROLLBAR);
  lv_obj_add_event_cb(nodes_table_body, Nodes_TableScrolled, LV_EVENT_ALL, NULL);

  nodes_spacer = lv_obj_create(nodes_table_body);
  lv_obj_remove_style_all(nodes_spacer);
  lv_obj_set_pos(nodes_spacer, 0, 0);
  lv_obj_set_size(nodes_spacer, 1, UI_NODE_TABLE_HEIGHT);
  lv_obj_set_clickable(nodes_spacer, false);
  lv_obj_set_scrollable(nodes_spacer, false);

  for (row = 0U; row < UI_NODE_VISIBLE_ROWS; ++row)
  {
    lv_obj_t *open_label;
    int32_t y = (int32_t)(row * UI_NODE_ROW_HEIGHT) + 4;
    Clear_NodeRow(&node_row_text[row]);
    node_cell_labels[row][0] = Create_Label(nodes_table_body, 4, y, 32, "", lv_color_hex(0xE8EEF2));
    lv_obj_set_clickable(node_cell_labels[row][0], true);
    lv_obj_add_event_cb(node_cell_labels[row][0], Log_NodeToggleClicked,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)row);
    node_cell_labels[row][1] = Create_Label(nodes_table_body, 40, y, 65, "", lv_color_hex(0xE8EEF2));
    node_cell_labels[row][2] = Create_Label(nodes_table_body, 111, y, 55, "", lv_color_hex(0xE8EEF2));
    node_cell_labels[row][3] = Create_Label(nodes_table_body, 172, y, 54, "", lv_color_hex(0xE8EEF2));
    node_cell_labels[row][4] = Create_Label(nodes_table_body, 232, y, 54, "", lv_color_hex(0xE8EEF2));
    node_cell_labels[row][5] = Create_Label(nodes_table_body, 292, y, 140, "", lv_color_hex(0xA9C7D7));
    lv_label_set_text_static(node_cell_labels[row][0], node_row_text[row].node_id);
    lv_label_set_text_static(node_cell_labels[row][1], node_row_text[row].last_seen);
    lv_label_set_text_static(node_cell_labels[row][2], node_row_text[row].frames);
    lv_label_set_text_static(node_cell_labels[row][3], node_row_text[row].traffic);
    lv_label_set_text_static(node_cell_labels[row][4], node_row_text[row].response);
    lv_label_set_text_static(node_cell_labels[row][5], node_row_text[row].type);

    node_open_buttons[row] = lv_button_create(nodes_table_body);
    lv_obj_set_pos(node_open_buttons[row], 438, y - 3);
    lv_obj_set_size(node_open_buttons[row], 38, 21);
    lv_obj_set_style_pad_all(node_open_buttons[row], 0, 0);
    lv_obj_set_style_radius(node_open_buttons[row], 3, 0);
    lv_obj_add_event_cb(node_open_buttons[row], Open_NodeEvents, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)row);
    open_label = lv_label_create(node_open_buttons[row]);
    lv_label_set_text(open_label, LV_SYMBOL_RIGHT);
    lv_obj_center(open_label);
    lv_obj_set_hidden(node_open_buttons[row], true);
  }

  Create_GridLine(nodes_screen, 0, 35, LCD_WIDTH, 1);
  Create_GridLine(nodes_screen, 0, 58, LCD_WIDTH, 1);
  Create_GridLine(nodes_screen, 38, 36, 1, 236);
  Create_GridLine(nodes_screen, 109, 36, 1, 236);
  Create_GridLine(nodes_screen, 170, 36, 1, 236);
  Create_GridLine(nodes_screen, 230, 36, 1, 236);
  Create_GridLine(nodes_screen, 290, 36, 1, 236);
  Create_GridLine(nodes_screen, 436, 36, 1, 236);
  for (line = 1U; line < UI_NODE_VISIBLE_ROWS; ++line)
  {
    Create_GridLine(nodes_screen, 0, UI_NODE_TABLE_Y + (int32_t)(line * UI_NODE_ROW_HEIGHT),
                    LCD_WIDTH, 1);
  }

  (void)Create_NavigationButton(node_events_screen, 4, 3, 44, 34, LV_SYMBOL_LEFT, nodes_screen);
  node_events_title_label = Create_Label(node_events_screen, 54, 10, 302,
                                         "Node transactions", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(node_events_title_label, &lv_font_montserrat_14, 0);

  mode_button = lv_button_create(node_events_screen);
  lv_obj_set_size(mode_button, 56, 34);
  lv_obj_set_pos(mode_button, 360, 1);
  lv_obj_set_style_pad_all(mode_button, 0, 0);
  lv_obj_add_event_cb(mode_button, NodeEvents_ModeClicked, LV_EVENT_CLICKED, NULL);
  node_events_mode_label = lv_label_create(mode_button);
  lv_label_set_text(node_events_mode_label, "ALL");
  lv_obj_center(node_events_mode_label);

  pause_button = lv_button_create(node_events_screen);
  lv_obj_set_size(pause_button, 56, 34);
  lv_obj_set_pos(pause_button, 420, 1);
  lv_obj_set_style_pad_all(pause_button, 0, 0);
  lv_obj_add_event_cb(pause_button, NodeEvents_PauseClicked, LV_EVENT_CLICKED, NULL);
  node_events_pause_label = lv_label_create(pause_button);
  lv_label_set_text(node_events_pause_label, LV_SYMBOL_PAUSE);
  lv_obj_center(node_events_pause_label);

  header_panel = lv_obj_create(node_events_screen);
  lv_obj_remove_style_all(header_panel);
  lv_obj_set_pos(header_panel, 0, 36);
  lv_obj_set_size(header_panel, LCD_WIDTH, 23);
  lv_obj_set_style_bg_color(header_panel, lv_color_hex(0x17242B), 0);
  lv_obj_set_style_bg_opa(header_panel, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(header_panel, false);
  (void)Create_TableHeader(header_panel, 2, 80, "Time", Event_HeaderClicked, EVENT_SORT_TIME);
  (void)Create_TableHeader(header_panel, 86, 58, "Lat us", Event_HeaderClicked, EVENT_SORT_LATENCY);
  (void)Create_TableHeader(header_panel, 148, 44, "ID", Event_HeaderClicked, EVENT_SORT_ID);
  node_events_type_header_label = lv_obj_get_child(
    Create_TableHeader(header_panel, 196, 56, "Type", Event_HeaderClicked, EVENT_SORT_TYPE), 0);
  (void)Create_TableHeader(header_panel, 256, 44, "Obj", Event_HeaderClicked, EVENT_SORT_OBJECT);
  (void)Create_TableHeader(header_panel, 304, 30, "Sub", Event_HeaderClicked, EVENT_SORT_SUBINDEX);
  (void)Create_TableHeader(header_panel, 338, 142, "Data (LSB)", Event_HeaderClicked, EVENT_SORT_DATA);

  node_events_table_body = lv_obj_create(node_events_screen);
  lv_obj_remove_style_all(node_events_table_body);
  lv_obj_set_pos(node_events_table_body, 0, UI_EVENT_TABLE_Y);
  lv_obj_set_size(node_events_table_body, LCD_WIDTH, UI_EVENT_TABLE_HEIGHT);
  lv_obj_set_style_bg_color(node_events_table_body, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(node_events_table_body, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(node_events_table_body, 0, 0);
  lv_obj_set_scroll_dir(node_events_table_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(node_events_table_body, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(node_events_table_body, lv_color_hex(0x65C7F7), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(node_events_table_body, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(node_events_table_body, 4, LV_PART_SCROLLBAR);
  lv_obj_add_event_cb(node_events_table_body, Events_TableScrolled, LV_EVENT_ALL, NULL);

  node_events_spacer = lv_obj_create(node_events_table_body);
  lv_obj_remove_style_all(node_events_spacer);
  lv_obj_set_pos(node_events_spacer, 0, 0);
  lv_obj_set_size(node_events_spacer, 1, UI_EVENT_TABLE_HEIGHT);
  lv_obj_set_clickable(node_events_spacer, false);
  lv_obj_set_scrollable(node_events_spacer, false);

  for (row = 0U; row < UI_EVENT_VISIBLE_ROWS; ++row)
  {
    int32_t y = (int32_t)(row * UI_EVENT_ROW_HEIGHT) + 3;
    Clear_EventRow(&event_row_text[row]);
    event_cell_labels[row][0] = Create_Label(node_events_table_body, 4, y, 78, "", lv_color_hex(0xE8EEF2));
    event_cell_labels[row][1] = Create_Label(node_events_table_body, 86, y, 58, "", lv_color_hex(0xA9C7D7));
    event_cell_labels[row][2] = Create_Label(node_events_table_body, 148, y, 44, "", lv_color_hex(0xE8EEF2));
    event_cell_labels[row][3] = Create_Label(node_events_table_body, 196, y, 56, "", lv_color_hex(0xA9C7D7));
    event_cell_labels[row][4] = Create_Label(node_events_table_body, 256, y, 44, "", lv_color_hex(0xA9C7D7));
    event_cell_labels[row][5] = Create_Label(node_events_table_body, 304, y, 30, "", lv_color_hex(0xA9C7D7));
    event_cell_labels[row][6] = Create_Label(node_events_table_body, 338, y, 138, "", lv_color_hex(0xE8EEF2));
    lv_label_set_text_static(event_cell_labels[row][0], event_row_text[row].time);
    lv_label_set_text_static(event_cell_labels[row][1], event_row_text[row].latency);
    lv_label_set_text_static(event_cell_labels[row][2], event_row_text[row].id);
    lv_label_set_text_static(event_cell_labels[row][3], event_row_text[row].type);
    lv_label_set_text_static(event_cell_labels[row][4], event_row_text[row].object);
    lv_label_set_text_static(event_cell_labels[row][5], event_row_text[row].subindex);
    lv_label_set_text_static(event_cell_labels[row][6], event_row_text[row].data);
  }

  Create_GridLine(node_events_screen, 0, 35, LCD_WIDTH, 1);
  Create_GridLine(node_events_screen, 0, 58, LCD_WIDTH, 1);
  Create_GridLine(node_events_screen, 84, 36, 1, 236);
  Create_GridLine(node_events_screen, 146, 36, 1, 236);
  Create_GridLine(node_events_screen, 194, 36, 1, 236);
  Create_GridLine(node_events_screen, 254, 36, 1, 236);
  Create_GridLine(node_events_screen, 302, 36, 1, 236);
  Create_GridLine(node_events_screen, 336, 36, 1, 236);
  for (line = 1U; line < UI_EVENT_VISIBLE_ROWS; ++line)
  {
    Create_GridLine(node_events_screen, 0,
                    UI_EVENT_TABLE_Y + (int32_t)(line * UI_EVENT_ROW_HEIGHT),
                    LCD_WIDTH, 1);
  }

  screen = monitor_screen;
  (void)Create_NavigationButton(screen, 4, 1, 42, 34, LV_SYMBOL_HOME, main_screen);
  status_label = lv_label_create(screen);
  lv_obj_set_pos(status_label, 52, 7);
  lv_obj_set_width(status_label, 284);
  lv_label_set_text(status_label, "CAN SCAN 1000 kbit/s");

  monitor_log_button = Create_QuickLogButton(screen, 342, &monitor_log_button_label);

  pause_button = lv_button_create(screen);
  lv_obj_set_size(pause_button, 52, 34);
  lv_obj_set_pos(pause_button, 424, 1);
  lv_obj_add_event_cb(pause_button, Pause_ButtonClicked, LV_EVENT_CLICKED, NULL);
  pause_button_label = lv_label_create(pause_button);
  lv_obj_center(pause_button_label);
  lv_label_set_text(pause_button_label, LV_SYMBOL_PAUSE);

  header_panel = lv_obj_create(screen);
  lv_obj_remove_style_all(header_panel);
  lv_obj_set_pos(header_panel, 0, 37);
  lv_obj_set_size(header_panel, LCD_WIDTH, 19);
  lv_obj_set_style_bg_color(header_panel, lv_color_hex(0x17242B), 0);
  lv_obj_set_style_bg_opa(header_panel, LV_OPA_COVER, 0);
  lv_obj_set_clickable(header_panel, false);
  lv_obj_set_scrollable(header_panel, false);

  (void)Create_Label(header_panel, UI_COL_TIME_X, 2, 76, "Time", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_ID_X, 2, 58, "ID", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_FORMAT_X, 2, 34, "FMT", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_TYPE_X, 2, 46, "TYPE", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_LENGTH_X, 2, 24, "LEN", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_DATA_X, 2, 214, "Data bytes", lv_color_hex(0x65C7F7));

  table_body = lv_obj_create(screen);
  lv_obj_remove_style_all(table_body);
  lv_obj_set_pos(table_body, 0, UI_TABLE_Y);
  lv_obj_set_size(table_body, LCD_WIDTH, UI_TABLE_HEIGHT);
  lv_obj_set_style_bg_color(table_body, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(table_body, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(table_body, 0, 0);
  lv_obj_set_scroll_dir(table_body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(table_body, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(table_body, lv_color_hex(0x65C7F7), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(table_body, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(table_body, 4, LV_PART_SCROLLBAR);
  lv_obj_add_event_cb(table_body, Table_Scrolled, LV_EVENT_ALL, NULL);

  history_spacer = lv_obj_create(table_body);
  lv_obj_remove_style_all(history_spacer);
  lv_obj_set_pos(history_spacer, 0, 0);
  lv_obj_set_size(history_spacer, 1, UI_TABLE_HEIGHT);
  lv_obj_set_clickable(history_spacer, false);
  lv_obj_set_scrollable(history_spacer, false);

  for (row = 0U; row < UI_VISIBLE_ROWS; ++row)
  {
    int32_t y = (int32_t)(row * UI_ROW_HEIGHT) + 2;
    Clear_Row(&row_text[row]);
    cell_labels[row][0] = Create_Label(table_body, UI_COL_TIME_X, y, 76, "", lv_color_hex(0xE8EEF2));
    cell_labels[row][1] = Create_Label(table_body, UI_COL_ID_X, y, 58, "", lv_color_hex(0xE8EEF2));
    cell_labels[row][2] = Create_Label(table_body, UI_COL_FORMAT_X, y, 34, "", lv_color_hex(0xA9C7D7));
    cell_labels[row][3] = Create_Label(table_body, UI_COL_TYPE_X, y, 46, "", lv_color_hex(0xA9C7D7));
    cell_labels[row][4] = Create_Label(table_body, UI_COL_LENGTH_X, y, 24, "", lv_color_hex(0xA9C7D7));
    cell_labels[row][5] = Create_Label(table_body, UI_COL_DATA_X, y, 214, "", lv_color_hex(0xE8EEF2));
    lv_label_set_text_static(cell_labels[row][0], row_text[row].time);
    lv_label_set_text_static(cell_labels[row][1], row_text[row].id);
    lv_label_set_text_static(cell_labels[row][2], row_text[row].format);
    lv_label_set_text_static(cell_labels[row][3], row_text[row].type);
    lv_label_set_text_static(cell_labels[row][4], row_text[row].length);
    lv_label_set_text_static(cell_labels[row][5], row_text[row].data);
  }

  Create_GridLine(screen, 0, 37, LCD_WIDTH, 1);
  Create_GridLine(screen, 0, 55, LCD_WIDTH, 1);
  Create_GridLine(screen, 0, 271, LCD_WIDTH, 1);
  Create_GridLine(screen, 82, 37, 1, 235);
  Create_GridLine(screen, 144, 37, 1, 235);
  Create_GridLine(screen, 182, 37, 1, 235);
  Create_GridLine(screen, 232, 37, 1, 235);
  Create_GridLine(screen, 258, 37, 1, 235);
  for (line = 1U; line < UI_VISIBLE_ROWS; ++line)
  {
    Create_GridLine(screen, 0, UI_TABLE_Y + (int32_t)(line * UI_ROW_HEIGHT), LCD_WIDTH, 1);
  }

  history_write = 0U;
  history_count = 0U;
  display_paused = false;
  programmatic_scroll = false;
  node_snapshot_count = 0U;
  node_sort_key = NODE_SORT_ID;
  node_sort_ascending = true;
  nodes_programmatic_scroll = false;
  event_snapshot_count = 0U;
  selected_node_id = 0U;
  event_sort_key = EVENT_SORT_TIME;
  event_sort_ascending = false;
  events_programmatic_scroll = false;
  node_events_paused = false;
  node_object_mode = false;
  scanner_snapshot_count = 0U;
  scanner_programmatic_scroll = false;
  rows_dirty = false;
  Refresh_LogStatus();
  lv_screen_load(main_screen);
}

void Ui_ProcessFrame(const CanFrameRecord *frame)
{
  if ((frame == NULL) || display_paused)
  {
    return;
  }

  history[history_write & UI_HISTORY_MASK] = *frame;
  ++history_write;
  if (history_count < UI_HISTORY_LENGTH)
  {
    ++history_count;
  }
  rows_dirty = true;
}

void Ui_TaskLoop(void)
{
  static uint32_t last_status_update;
  static uint32_t last_analyzer_update;
  CanCaptureStatus status;
  CanTxStatus tx;
  uint32_t now = HAL_GetTick();

  CanScanner_Update();

  if (display_backlight_on &&
      ((now - display_last_touch_ms) >= DISPLAY_IDLE_TIMEOUT_MS))
  {
    Display_SetBacklight(false);
  }

  if (rows_dirty)
  {
    uint32_t content_height = history_count * UI_ROW_HEIGHT;
    if (content_height < UI_TABLE_HEIGHT)
    {
      content_height = UI_TABLE_HEIGHT;
    }
    lv_obj_set_height(history_spacer, (int32_t)content_height);
    Render_Rows();
    rows_dirty = false;
  }

  if ((now - last_status_update) >= 250U)
  {
    char text[96];
    char main_text[112];
    const char *state;
    const char *operating;
    bool bridge_active;
    CanScannerStatus scanner_status;
    uint32_t drops;
    CanControl_GetStatus(&status);
    CanTx_GetStatus(&tx);
    CanScanner_GetStatus(&scanner_status);
    Refresh_ErrorStatistics(&status);
    bridge_active = CanBridge_IsActive();
    state = (status.state == CAN_SYNC_LOCKED) ? "LOCK" :
            ((status.state == CAN_SYNC_MANUAL) ? "MAN" : "SCAN");
    operating = scanner_status.running ? "SCANNER/" :
                (bridge_active ? "BRIDGE/" :
                (tx.periodic_active ? "SDO TIMER/" : ""));
    lv_obj_set_hidden(bridge_indicator_label,
                      !bridge_active || (lv_screen_active() != main_screen));
    lv_obj_set_hidden(scanner_indicator_label,
                      !scanner_status.running ||
                      (lv_screen_active() == scanner_screen));
    drops = status.ring_overflows + status.gui_overflows +
            CanopenAnalyzer_GetQueueOverflows();
    (void)snprintf(text, sizeof(text), "CAN %s%s%s %luk RX:%lu D:%lu E:%lu",
                   operating, display_paused ? "PAUSE/" : "", state,
                   (unsigned long)status.bitrate_kbps,
                   (unsigned long)status.received,
                   (unsigned long)drops,
                   (unsigned long)status.errors);
    (void)snprintf(main_text, sizeof(main_text),
                   "CAN %s%s %lu k  LOAD:%lu.%lu%%  RX:%lu  DROP:%lu  ERR:%lu",
                   operating, state, (unsigned long)status.bitrate_kbps,
                   (unsigned long)(status.bus_load_permille / 10U),
                   (unsigned long)(status.bus_load_permille % 10U),
                   (unsigned long)status.received, (unsigned long)drops,
                   (unsigned long)status.errors);
    lv_label_set_text(status_label, text);
    lv_label_set_text(main_status_label, main_text);
    lv_obj_set_style_text_color(status_label,
      lv_color_hex((scanner_status.running || bridge_active || tx.periodic_active) ?
                   0xC084FC : 0xE8EEF2), 0);
    lv_obj_set_style_text_color(main_status_label,
      lv_color_hex((scanner_status.running || bridge_active || tx.periodic_active) ?
                   0xC084FC : 0xA9C7D7), 0);
    Refresh_LogStatus();
    Tx_RefreshStatus();
    last_status_update = now;
  }

  if ((now - last_analyzer_update) >= 250U)
  {
    lv_obj_t *active_screen = lv_screen_active();
    if (active_screen == nodes_screen)
    {
      Refresh_NodesSnapshot();
    }
    else if ((active_screen == node_events_screen) && (selected_node_id != 0U))
    {
      if (node_events_paused)
      {
        Update_NodeEventsHeader();
      }
      else
      {
        Refresh_NodeEventsSnapshot();
      }
    }
    else if (active_screen == scanner_screen)
    {
      Refresh_Scanner();
    }
    last_analyzer_update = now;
  }

  (void)lv_timer_handler();
}
