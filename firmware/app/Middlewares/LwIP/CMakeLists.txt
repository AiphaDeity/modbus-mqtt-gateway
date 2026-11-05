# LwIP 协议栈（带 OS：NO_SYS=0 + LWIP_NETCONN=1/LWIP_SOCKET=1）
# 源文件来自正点原子探索者例程 lwip-2.2.0/src（协议栈本体，库无关）
# 带 OS 模式必须编译 src/api/（tcpip.c 提供 tcpip_init/tcpip_input，sockets/netconn 由应用用）
# 必须链接 stm32cubemx：lwipopts.h include 了 FreeRTOSConfig.h（在 Core/Inc），
# 协议栈编译时要搜得到，而 Core/Inc 的 include 路径由 stm32cubemx INTERFACE 传递
add_library(lwip STATIC
    # ---- API 层（LWIP_NETCONN=1 / LWIP_SOCKET=1 时必需）----
    src/api/api_lib.c
    src/api/api_msg.c
    src/api/err.c
    src/api/if_api.c
    src/api/netbuf.c
    src/api/netdb.c
    src/api/netifapi.c
    src/api/sockets.c
    src/api/tcpip.c

    # ---- 核心层 ----
    src/core/init.c
    src/core/def.c
    src/core/dns.c
    src/core/inet_chksum.c
    src/core/ip.c
    src/core/mem.c
    src/core/memp.c
    src/core/netif.c
    src/core/pbuf.c
    src/core/raw.c
    src/core/stats.c
    src/core/sys.c
    src/core/timeouts.c
    src/core/udp.c
    src/core/tcp.c
    src/core/tcp_in.c
    src/core/tcp_out.c

    # ---- IPv4 ----
    # ★必须全量编译（例程 Keil 工程就是全加）：lwip 2.2.0 里很多功能有编译开关，
    #   LWIP_DHCP=1 需要 dhcp.c、LWIP_DHCP 连带 ACD（地址冲突检测）需要 acd.c、
    #   LWIP_IP_REASSEMBLY=1（默认开）需要 ip4_frag.c——漏一个就是链接期一堆 undefined
    src/core/ipv4/acd.c
    src/core/ipv4/autoip.c
    src/core/ipv4/dhcp.c
    src/core/ipv4/etharp.c
    src/core/ipv4/icmp.c
    src/core/ipv4/igmp.c
    src/core/ipv4/ip4.c
    src/core/ipv4/ip4_addr.c
    src/core/ipv4/ip4_frag.c

    # ---- 网卡抽象 ----
    src/netif/ethernet.c
)
target_include_directories(lwip PUBLIC
    src/include                        # lwip 头文件在这里（lwip/opt.h、netif/ethernet.h）
    ${CMAKE_CURRENT_SOURCE_DIR}/../../bsp/ethernet   # 找 lwipopts.h
)
# 拿 stm32cubemx 的 include 路径（Core/Inc → FreeRTOSConfig.h）
target_link_libraries(lwip PUBLIC stm32cubemx)
