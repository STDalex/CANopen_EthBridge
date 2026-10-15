#include "ui.h"

#include "cmsis_os.h"
#include "dma2d.h"
#include "main.h"
#include "touch_ft5336.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define LCD_WIDTH              480U
#define LCD_HEIGHT             272U
#define LCD_FRAMEBUFFER        0xC0000000UL
#define UI_DRAW_LINES          24U
#define UI_VISIBLE_ROWS        12U
#define UI_HISTORY_LENGTH      256U
#define UI_HISTORY_MASK        (UI_HISTORY_LENGTH - 1U)
#define UI_ROW_HEIGHT          18U
#define UI_TABLE_Y             56U
#define UI_TABLE_HEIGHT        (UI_VISIBLE_ROWS * UI_ROW_HEIGHT)

#define UI_COL_TIME_X          4
#define UI_COL_ID_X            72
#define UI_COL_FORMAT_X        138
#define UI_COL_TYPE_X          176
#define UI_COL_LENGTH_X        226
#define UI_COL_DATA_X          252

typedef struct
{
  char time[12];
  char id[9];
  char format[4];
  char type[5];
  char length[2];
  char data[25];
} UiRowText;

static uint16_t draw_buffer[LCD_WIDTH * UI_DRAW_LINES] __attribute__((aligned(32)));
static lv_obj_t *main_screen;
static lv_obj_t *monitor_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *nodes_screen;
static lv_obj_t *status_label;
static lv_obj_t *main_status_label;
static lv_obj_t *settings_status_label;
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

static void Bitrate_Changed(lv_event_t *event)
{
  static const uint32_t rates[] = {0U, 1000U, 800U, 500U, 250U, 125U, 100U, 50U, 20U, 10U};
  lv_obj_t *dropdown = lv_event_get_target_obj(event);
  uint32_t selected = lv_dropdown_get_selected(dropdown);
  if (selected < (sizeof(rates) / sizeof(rates[0])))
  {
    CanControl_RequestBitrate(rates[selected]);
  }
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

  (void)snprintf(text->time, sizeof(text->time), "%5lu.%03lu",
                 (unsigned long)(frame->timestamp_us / 1000000U),
                 (unsigned long)((frame->timestamp_us / 1000U) % 1000U));
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

void Ui_Init(void)
{
  lv_display_t *display;
  lv_indev_t *touch;
  lv_obj_t *screen;
  lv_obj_t *header_panel;
  lv_obj_t *dropdown;
  lv_obj_t *pause_button;
  lv_obj_t *title;
  lv_obj_t *info;
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
  Style_Screen(main_screen);
  Style_Screen(monitor_screen);
  Style_Screen(settings_screen);
  Style_Screen(nodes_screen);

  title = Create_Label(main_screen, 0, 14, LCD_WIDTH, "CAN Sniffer", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  main_status_label = Create_Label(main_screen, 20, 42, 440, "CAN starting...", lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(main_status_label, LV_TEXT_ALIGN_CENTER, 0);
  (void)Create_NavigationButton(main_screen, 12, 72, 144, 150, "CAN\nMonitor", monitor_screen);
  (void)Create_NavigationButton(main_screen, 168, 72, 144, 150, "Settings", settings_screen);
  (void)Create_NavigationButton(main_screen, 324, 72, 144, 150, "Nodes\nStage 2", nodes_screen);

  (void)Create_NavigationButton(settings_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(settings_screen, 58, 10, 410, "Settings", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  settings_status_label = Create_Label(settings_screen, 20, 52, 440, "CAN starting...", lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(settings_status_label, LV_TEXT_ALIGN_CENTER, 0);
  (void)Create_Label(settings_screen, 48, 102, 112, "CAN bitrate", lv_color_hex(0xE8EEF2));
  dropdown = lv_dropdown_create(settings_screen);
  lv_dropdown_set_options(dropdown, "Auto\n1000\n800\n500\n250\n125\n100\n50\n20\n10");
  lv_obj_set_size(dropdown, 180, 42);
  lv_obj_set_pos(dropdown, 174, 86);
  lv_obj_add_event_cb(dropdown, Bitrate_Changed, LV_EVENT_VALUE_CHANGED, NULL);
  info = Create_Label(settings_screen, 40, 155, 400,
                      "Default: Auto\nSelection is kept until restart.\nAuto scans from 1000 kbit/s down.",
                      lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);

  (void)Create_NavigationButton(nodes_screen, 4, 3, 44, 34, LV_SYMBOL_HOME, main_screen);
  title = Create_Label(nodes_screen, 58, 10, 410, "Nodes / frame decode", lv_color_hex(0x65C7F7));
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  info = Create_Label(nodes_screen, 40, 82, 400,
                      "Stage 2\n\nUnique node_id detection\nand CAN frame parsing\n\nNot implemented yet",
                      lv_color_hex(0xA9C7D7));
  lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);

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

  (void)Create_Label(header_panel, UI_COL_TIME_X, 2, 62, "Time", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_ID_X, 2, 60, "ID", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_FORMAT_X, 2, 34, "FMT", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_TYPE_X, 2, 46, "TYPE", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_LENGTH_X, 2, 24, "LEN", lv_color_hex(0x65C7F7));
  (void)Create_Label(header_panel, UI_COL_DATA_X, 2, 224, "Data bytes", lv_color_hex(0x65C7F7));

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
    cell_labels[row][0] = Create_Label(table_body, UI_COL_TIME_X, y, 62, "", lv_color_hex(0xE8EEF2));
    cell_labels[row][1] = Create_Label(table_body, UI_COL_ID_X, y, 60, "", lv_color_hex(0xE8EEF2));
    cell_labels[row][2] = Create_Label(table_body, UI_COL_FORMAT_X, y, 34, "", lv_color_hex(0xA9C7D7));
    cell_labels[row][3] = Create_Label(table_body, UI_COL_TYPE_X, y, 46, "", lv_color_hex(0xA9C7D7));
    cell_labels[row][4] = Create_Label(table_body, UI_COL_LENGTH_X, y, 24, "", lv_color_hex(0xA9C7D7));
    cell_labels[row][5] = Create_Label(table_body, UI_COL_DATA_X, y, 224, "", lv_color_hex(0xE8EEF2));
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
  Create_GridLine(screen, 68, 37, 1, 235);
  Create_GridLine(screen, 134, 37, 1, 235);
  Create_GridLine(screen, 172, 37, 1, 235);
  Create_GridLine(screen, 222, 37, 1, 235);
  Create_GridLine(screen, 248, 37, 1, 235);
  for (line = 1U; line < UI_VISIBLE_ROWS; ++line)
  {
    Create_GridLine(screen, 0, UI_TABLE_Y + (int32_t)(line * UI_ROW_HEIGHT), LCD_WIDTH, 1);
  }

  history_write = 0U;
  history_count = 0U;
  display_paused = false;
  programmatic_scroll = false;
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
                   (unsigned long)(status.ring_overflows + status.gui_overflows),
                   (unsigned long)status.errors);
    lv_label_set_text(status_label, text);
    lv_label_set_text(main_status_label, text);
    lv_label_set_text(settings_status_label, text);
    last_status_update = now;
  }

  (void)lv_timer_handler();
}
