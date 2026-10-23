#ifndef MEMORY_LAYOUT_H
#define MEMORY_LAYOUT_H

/* STM32F746G-DISCO external SDRAM: 0xC0000000..0xC07FFFFF.
   The LTDC framebuffer starts at 0xC0000000 and occupies 261120 bytes.
   The next 256 KiB are reserved for the LVGL heap. The remainder of the
   first MiB stays unused, followed by the logger pool and RTOS work areas. */
#define LVGL_HEAP_BASE                       0xC0040000UL
#define LVGL_HEAP_BYTES                      0x00040000UL
#define CAN_LOG_SDRAM_BLOCK_POOL_BASE       0xC0100000UL
#define CAN_LOG_SDRAM_BLOCK_POOL_BYTES      (2UL * 1024UL * 1024UL)
#define STORAGE_TASK_STACK_WORDS            2048U
#define CAN_LOG_FREE_QUEUE_STORAGE_BASE     0xC0302000UL
#define CAN_LOG_READY_QUEUE_STORAGE_BASE    0xC0302100UL
#define CAN_LOG_IO_BUFFER_BASE              0xC0303000UL
#define CAN_LOG_JSON_BUFFER_BASE            0xC0303200UL
#define GUI_TASK_STACK_BASE                  0xC0304000UL
#define GUI_TASK_STACK_WORDS                 4096U
#define NETWORK_TASK_STACK_BASE              0xC0308000UL
#define NETWORK_TASK_STACK_WORDS             1024U
#define API_TASK_STACK_BASE                  0xC0309000UL
#define API_TASK_STACK_WORDS                 2048U
#define API_SNAPSHOT_BASE                    0xC030B000UL
#define CAN_LOG_LIST_BUFFER_BASE             0xC0313000UL
#define CANOPEN_ANALYZER_STATE_BASE          0xC0314000UL
#define UI_SNAPSHOT_BASE                     0xC0324000UL
#define UI_SNAPSHOT_REGION_BYTES             0x00010000UL
#define WEBSOCKET_TASK_STACK_BASE             0xC0334000UL
#define WEBSOCKET_TASK_STACK_WORDS            2048U
#define WEBSOCKET_RING_BASE                   0xC0340000UL
#define WEBSOCKET_RING_REGION_BYTES           0x00040000UL
#define CAN_LOG_DOWNLOAD_BUFFER_BASE          0xC0380000UL
#define CAN_LOG_DOWNLOAD_BUFFER_BYTES         0x00004000UL
#define API_HTTP_REQUEST_BUFFER_BASE           0xC0384000UL
#define API_HTTP_REQUEST_BUFFER_BYTES          0x00004000UL
#define BRIDGE_TASK_STACK_BASE                  0xC0388000UL
#define BRIDGE_TASK_STACK_WORDS                 2048U
#define LWIP_HEAP_BASE                          0xC038A000UL
#define LWIP_HEAP_BYTES                         0x00010000UL

#endif
