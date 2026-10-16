#include "ui.h"

#include "canopen_analyzer.h"
#include "can_pipeline.h"
#include "cmsis_os.h"
#include "dma2d.h"
#include "main.h"
#include "touch_ft5336.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_cyrillic_14)

#define LCD_WIDTH              480U
#define LCD_HEIGHT             272U
#define LCD_FRAMEBUFFER        0xC0000000UL
#define CAN_SNIFFER_VERSION    "2.5.5"
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
  char node_id[4];
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

static uint16_t draw_buffer[LCD_WIDTH * UI_DRAW_LINES] __attribute__((aligned(32)));
static lv_obj_t *main_screen;
static lv_obj_t *monitor_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *nodes_screen;
static lv_obj_t *node_events_screen;
static lv_obj_t *info_screen;
static lv_obj_t *status_label;
static lv_obj_t *main_status_label;
static lv_obj_t *pause_button_label;
static lv_obj_t *table_body;
static lv_obj_t *history_spacer;
static lv_obj_t *cell_labels[UI_VISIBLE_ROWS][6];
static UiRowText row_text[UI_VISIBLE_ROWS];
static CanFrameRecord history[UI_HISTORY_LENGTH];
static uint32_t history_write;
static uint32_t history_count;
static bool rows_dirty;
static bool display_paused;
static bool programmatic_scroll;
static lv_obj_t *nodes_title_label;
static lv_obj_t *nodes_table_body;
static lv_obj_t *nodes_spacer;
static lv_obj_t *node_cell_labels[UI_NODE_VISIBLE_ROWS][6];
static lv_obj_t *node_open_buttons[UI_NODE_VISIBLE_ROWS];
static UiNodeRowText node_row_text[UI_NODE_VISIBLE_ROWS];
static CanopenNodeSnapshot node_snapshot[CANOPEN_NODE_COUNT_MAX];
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
static lv_obj_t *event_cell_labels[UI_EVENT_VISIBLE_ROWS][7];
static UiEventRowText event_row_text[UI_EVENT_VISIBLE_ROWS];
static CanopenNodeEventSnapshot event_snapshot[CANOPEN_EVENT_SNAPSHOT_MAX];
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

static const char info_monitor_text[] =
  "Монитор: все принятые CAN-кадры с временем, ID, форматом, DLC и данными. "
  "Пауза останавливает таблицу, но прием продолжается.";
static const char info_nodes_text[] =
  "Ноды: список уникальных CANopen-нод, доля трафика и процент ответов SDO. "
  "Стрелка открывает пакеты выбранной ноды.";
static const char info_objects_text[] =
  "Объекты: уникальные пары SDO object/subindex. Запрос расположен перед ответом; "
  "время, данные и задержка обновляются в строке.";
static const char info_bitrate_text[] =
  "Скорость: Auto перебирает поддерживаемые скорости. Ручной режим фиксирует "
  "выбранное значение. CAN работает только на прием.";
static const char info_packet_types_text[] =
  "Типы: NMT - управление сетью; SYNC - синхронизация; EMCY - авария; "
  "TIME - время; TPDO - данные от ноды; RPDO - команды ноде; SDO - доступ к объектам; "
  "HB/Guard - контроль состояния; LSS - настройка ноды.";

static void Refresh_NodesSnapshot(void);
static void Refresh_NodeEventsSnapshot(void);
static void Render_Rows(void);

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

static void Touch_Read(lv_indev_t *indev, lv_indev_data_t *data)
{
  uint16_t x = 0U;
  uint16_t y = 0U;
  bool pressed = false;
  (void)indev;

  (void)TouchFT5336_Read(&x, &y, &pressed);
  data->point.x = (int32_t)x;
  data->point.y = (int32_t)y;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void Bitrate_SelectorClicked(lv_event_t *event)
{
  (void)event;
  if (lv_obj_is_hidden(bitrate_options_panel))
  {
    lv_obj_set_hidden(bitrate_options_panel, false);
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

  if (option != NULL)
  {
    lv_label_set_text(bitrate_selector_label, option->selector_text);
    CanControl_RequestBitrate(option->bitrate_kbps);
  }
  lv_obj_set_hidden(bitrate_options_panel, true);
  lv_obj_invalidate(settings_screen);
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
  if (target != NULL)
  {
    if (target == monitor_screen)
    {
      rows_dirty = true;
    }
    else if (target == nodes_screen)
    {
      Refresh_NodesSnapshot();
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
                 ((frame->flags & CAN_FRAME_FLAG_REMOTE) != 0U) ? "RTR" : "DATA");
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
      lv_obj_set_y(cell_labels[row][column], row_y);
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
      if ((left->sdo_request_count == 0U) && (right->sdo_request_count == 0U))
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
  (void)snprintf(text->node_id, sizeof(text->node_id), "%u", node->node_id);
  (void)snprintf(text->last_seen, sizeof(text->last_seen), "%5lu.%03lu",
                 (unsigned long)(node->last_seen_us / 1000000U),
                 (unsigned long)((node->last_seen_us / 1000U) % 1000U));
  (void)snprintf(text->frames, sizeof(text->frames), "%lu", (unsigned long)node->frame_count);
  (void)snprintf(text->traffic, sizeof(text->traffic), "%lu.%lu%%",
                 (unsigned long)(traffic_permille / 10U),
                 (unsigned long)(traffic_permille % 10U));
  if (node->sdo_request_count == 0U)
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
  (void)snprintf(title, sizeof(title), "Nodes  %5lu.%05lu s  Unique:%lu",
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

static void Format_EventRow(UiEventRowText *text, const CanopenNodeEventSnapshot *event)
{
  uint32_t i;
  uint32_t offset = 0U;
  uint32_t data_start = 0U;
  CanopenSdoAddress address = CanopenDecoder_DecodeSdoAddress(&event->frame, event->type);

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
    for (i = data_start; (i < event->frame.dlc) && (offset < (sizeof(text->data) - 3U)); ++i)
    {
      offset += (uint32_t)snprintf(&text->data[offset], sizeof(text->data) - offset,
                                   (i == 0U) ? "%02X" : " %02X", event->frame.data[i]);
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
      lv_obj_set_y(event_cell_labels[row][column], y);
      lv_obj_invalidate(event_cell_labels[row][column]);
    }
  }
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
  (void)snprintf(title, sizeof(title), "Node %u  %5lu.%05lu s  #%lu", selected_node_id,
                 (unsigned long)(analyzer_snapshot.last_timestamp_us / 1000000U),
                 (unsigned long)((analyzer_snapshot.last_timestamp_us / 10U) % 100000U),
                 (unsigned long)event_snapshot_count);
  lv_label_set_text(node_events_title_label, title);
  Render_EventRows();
}

static void Update_NodeEventsHeader(void)
{
  char title[48];
  CanopenAnalyzer_GetGlobalSnapshot(&analyzer_snapshot);
  (void)snprintf(title, sizeof(title), "Node %u  %5lu.%05lu s  #%lu", selected_node_id,
                 (unsigned long)(analyzer_snapshot.last_timestamp_us / 1000000U),
                 (unsigned long)((analyzer_snapshot.last_timestamp_us / 10U) % 100000U),
                 (unsigned long)event_snapshot_count);
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

void Ui_Init(void)
{
  lv_display_t *display;
  lv_indev_t *touch;
  lv_obj_t *screen;
  lv_obj_t *header_panel;
  lv_obj_t *bitrate_selector;
  lv_obj_t *bitrate_option_button;
  lv_obj_t *bitrate_option_label;
  lv_obj_t *reset_button;
  lv_obj_t *pause_button;
  lv_obj_t *mode_button;
  lv_obj_t *title;
  lv_obj_t *overview;
  lv_obj_t *version;
  uint32_t option;
  uint32_t row;
  uint32_t line;

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
  nodes_screen = lv_obj_create(NULL);
  node_events_screen = lv_obj_create(NULL);
  info_screen = lv_obj_create(NULL);
  Style_Screen(main_screen);
  Style_Screen(monitor_screen);
  Style_Screen(settings_screen);
  Style_Screen(nodes_screen);
  Style_Screen(node_events_screen);
  Style_Screen(info_screen);
  lv_obj_set_style_text_font(info_screen, &ui_font_cyrillic_14, 0);

  title = Create_Label(main_screen, 0, 14, LCD_WIDTH, "CAN Sniffer", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  main_status_label = Create_Label(main_screen, 20, 42, 440, "CAN starting...", lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(main_status_label, LV_TEXT_ALIGN_CENTER, 0);
  (void)Create_NavigationButton(main_screen, 24, 72, 208, 150, "CAN\nMonitor", monitor_screen);
  (void)Create_NavigationButton(main_screen, 248, 72, 208, 150, "Nodes", nodes_screen);
  reset_button = Create_NavigationButton(main_screen, 4, 3, 44, 34, LV_SYMBOL_REFRESH, main_screen);
  lv_obj_add_event_cb(reset_button, Reset_StatisticsClicked, LV_EVENT_CLICKED, NULL);
  (void)Create_NavigationButton(main_screen, 384, 3, 44, 34, "i", info_screen);
  (void)Create_NavigationButton(main_screen, 432, 3, 44, 34, LV_SYMBOL_SETTINGS, settings_screen);

  (void)Create_NavigationButton(info_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(info_screen, 58, 10, 410, "Информация", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &ui_font_cyrillic_14, 0);
  overview = Create_Label(info_screen, 16, 45, 448,
                          "Пассивный анализатор Classic CAN/CANopen: кадры в реальном времени, "
                          "статистика нод, объекты SDO и время ответа.",
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
  lv_label_set_text(bitrate_selector_label, bitrate_options[0].selector_text);
  lv_obj_set_style_text_color(bitrate_selector_label, lv_color_hex(0xF4FAFD), 0);
  lv_obj_center(bitrate_selector_label);

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

  (void)Create_NavigationButton(nodes_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  nodes_title_label = Create_Label(nodes_screen, 58, 10, 410, "Nodes  0.00000 s  Unique:0",
                                   lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(nodes_title_label, &lv_font_montserrat_14, 0);

  header_panel = lv_obj_create(nodes_screen);
  lv_obj_remove_style_all(header_panel);
  lv_obj_set_pos(header_panel, 0, 36);
  lv_obj_set_size(header_panel, LCD_WIDTH, 23);
  lv_obj_set_style_bg_color(header_panel, lv_color_hex(0x17242B), 0);
  lv_obj_set_style_bg_opa(header_panel, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(header_panel, false);
  (void)Create_TableHeader(header_panel, 2, 34, "Node", Node_HeaderClicked, NODE_SORT_ID);
  (void)Create_TableHeader(header_panel, 40, 67, "Last", Node_HeaderClicked, NODE_SORT_LAST_SEEN);
  (void)Create_TableHeader(header_panel, 111, 57, "Frames", Node_HeaderClicked, NODE_SORT_FRAMES);
  (void)Create_TableHeader(header_panel, 172, 56, "Traffic", Node_HeaderClicked, NODE_SORT_TRAFFIC);
  (void)Create_TableHeader(header_panel, 232, 56, "SDO %", Node_HeaderClicked, NODE_SORT_RESPONSE);
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
  (void)Create_TableHeader(header_panel, 196, 56, "Type", Event_HeaderClicked, EVENT_SORT_TYPE);
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
  lv_obj_set_width(status_label, 366);
  lv_label_set_text(status_label, "CAN SCAN 1000 kbit/s");

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
  rows_dirty = false;
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
  uint32_t now = HAL_GetTick();

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
    const char *state;
    CanControl_GetStatus(&status);
    state = (status.state == CAN_SYNC_LOCKED) ? "LOCK" :
            ((status.state == CAN_SYNC_MANUAL) ? "MAN" : "SCAN");
    (void)snprintf(text, sizeof(text), "CAN %s%s %lu k  RX:%lu  DROP:%lu  ERR:%lu",
                   display_paused ? "PAUSE/" : "", state, (unsigned long)status.bitrate_kbps,
                   (unsigned long)status.received,
                   (unsigned long)(status.ring_overflows + status.gui_overflows +
                                   CanopenAnalyzer_GetQueueOverflows()),
                   (unsigned long)status.errors);
    lv_label_set_text(status_label, text);
    lv_label_set_text(main_status_label, text);
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
    last_analyzer_update = now;
  }

  (void)lv_timer_handler();
}
