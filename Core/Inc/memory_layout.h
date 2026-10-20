#ifndef MEMORY_LAYOUT_H
#define MEMORY_LAYOUT_H

/* STM32F746G-DISCO external SDRAM: 0xC0000000..0xC07FFFFF.
   The LTDC framebuffer starts at 0xC0000000 and occupies 261120 bytes.
   Stage 3 deliberately leaves the remainder of the first MiB unused, then
   reserves a contiguous 2 MiB logger pool and its RTOS working area. */
#define CAN_LOG_SDRAM_BLOCK_POOL_BASE       0xC0100000UL
#define CAN_LOG_SDRAM_BLOCK_POOL_BYTES      (2UL * 1024UL * 1024UL)
#define STORAGE_TASK_STACK_WORDS            2048U
#define CAN_LOG_FREE_QUEUE_STORAGE_BASE     0xC0302000UL
#define CAN_LOG_READY_QUEUE_STORAGE_BASE    0xC0302100UL
#define CAN_LOG_IO_BUFFER_BASE              0xC0303000UL
#define CAN_LOG_JSON_BUFFER_BASE            0xC0303200UL
#define GUI_TASK_STACK_BASE                  0xC0304000UL
#define GUI_TASK_STACK_WORDS                 4096U

#endif
