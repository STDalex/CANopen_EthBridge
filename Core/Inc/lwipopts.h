#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* FreeRTOS / Netconn mode. */
#define NO_SYS                              0
#define SYS_LIGHTWEIGHT_PROT                1
#define LWIP_NETCONN                        1
#define LWIP_SOCKET                         0
#define LWIP_NETIF_API                      1

/* A dedicated, MPU non-cacheable 16 KiB area immediately below the DMA
   descriptor area. It is reserved by the Keil scatter file. */
#define MEM_ALIGNMENT                       4
#define MEM_SIZE                            (16U * 1024U)
#define LWIP_RAM_HEAP_POINTER               ((void *)0x20048000UL)

#define MEMP_NUM_PBUF                       12
#define MEMP_NUM_RAW_PCB                    2
#define MEMP_NUM_UDP_PCB                    4
#define MEMP_NUM_TCP_PCB                    4
#define MEMP_NUM_TCP_PCB_LISTEN             2
#define MEMP_NUM_TCP_SEG                    16
#define MEMP_NUM_NETCONN                    6
#define MEMP_NUM_SYS_TIMEOUT                12

#define PBUF_POOL_SIZE                      12
#define PBUF_POOL_BUFSIZE                   1524

#define LWIP_IPV4                           1
#define LWIP_IPV6                           0
#define LWIP_DHCP                           1
#define LWIP_AUTOIP                         0
#define LWIP_ICMP                           1
#define LWIP_UDP                            1
#define LWIP_TCP                            1
#define LWIP_DNS                            0

#define TCP_TTL                             64
#define TCP_QUEUE_OOSEQ                     0
#define TCP_MSS                             1460
#define TCP_SND_BUF                         (4U * TCP_MSS)
#define TCP_SND_QUEUELEN                    16
#define TCP_WND                             (4U * TCP_MSS)

#define LWIP_NETIF_HOSTNAME                 1
#define LWIP_NETIF_LINK_CALLBACK            1
#define LWIP_NETIF_STATUS_CALLBACK          1
#define LWIP_SO_RCVTIMEO                    1
#define LWIP_SO_SNDTIMEO                    1
#define LWIP_STATS                          0

/* STM32F746 Ethernet MAC inserts and verifies IPv4/TCP/UDP/ICMP checksums. */
#define CHECKSUM_GEN_IP                     0
#define CHECKSUM_GEN_UDP                    0
#define CHECKSUM_GEN_TCP                    0
#define CHECKSUM_GEN_ICMP                   0
#define CHECKSUM_CHECK_IP                   0
#define CHECKSUM_CHECK_UDP                  0
#define CHECKSUM_CHECK_TCP                  0
#define CHECKSUM_CHECK_ICMP                 0

#define TCPIP_THREAD_NAME                   "tcpip"
#define TCPIP_THREAD_STACKSIZE              1024
#define TCPIP_THREAD_PRIO                   osPriorityAboveNormal
#define TCPIP_MBOX_SIZE                     8
#define DEFAULT_UDP_RECVMBOX_SIZE           4
#define DEFAULT_TCP_RECVMBOX_SIZE           6
#define DEFAULT_ACCEPTMBOX_SIZE             4
#define DEFAULT_THREAD_STACKSIZE            768

#endif
