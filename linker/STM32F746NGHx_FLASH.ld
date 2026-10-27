/* GNU ld memory layout equivalent to MDK-ARM/CAN_sniffer.sct. */

ENTRY(Reset_Handler)

_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
  FLASH       (rx)  : ORIGIN = 0x08000000, LENGTH = 0x00100000
  RAM         (xrw) : ORIGIN = 0x20000000, LENGTH = 0x00040000
  SD_DMA      (xrw) : ORIGIN = 0x20040000, LENGTH = 0x00008000
  ETH_RX_DESC (xrw) : ORIGIN = 0x2004C000, LENGTH = 0x000000A0
  ETH_TX_DESC (xrw) : ORIGIN = 0x2004C0A0, LENGTH = 0x000000A0
  ETH_RX_POOL (xrw) : ORIGIN = 0x2004C140, LENGTH = 0x00003EC0
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > FLASH

  .ARM :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  .preinit_array (READONLY) :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > FLASH

  .init_array (READONLY) :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } > FLASH

  .fini_array (READONLY) :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  /* These sections must precede the generic .bss wildcard. */
  .sd_dma (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.bss.SdDmaBuffer))
    . = ALIGN(32);
  } > SD_DMA

  .eth_rx_desc (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.RxDecripSection))
    . = ALIGN(32);
  } > ETH_RX_DESC

  .eth_tx_desc (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.TxDecripSection))
    . = ALIGN(32);
  } > ETH_TX_DESC

  .eth_rx_pool (NOLOAD) :
  {
    . = ALIGN(32);
    KEEP(*(.RxPoolSection))
    . = ALIGN(32);
  } > ETH_RX_POOL

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > RAM

  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    __HeapBase = .;
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > RAM

  __HeapLimit = _estack - _Min_Stack_Size;

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

ASSERT(__HeapLimit >= (_end + _Min_Heap_Size), "RAM overflow: heap and stack do not fit")
