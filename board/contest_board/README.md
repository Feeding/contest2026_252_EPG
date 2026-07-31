# BK7258 板级适配（声网对话式 AI 开发套件 R1）

openvela / NuttX 在 **博通集成 Beken BK7258** 上的板级支持包，目标硬件为
**声网对话式 AI 开发套件 R1（Agora ConvoAI Kit R1）**。

## 一、为什么放在这里

openvela 支持完全 out-of-tree 的芯片 + 板级移植：`CONFIG_ARCH_CHIP_CUSTOM_DIR`
和 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 可以指向 `nuttx/` 之外的任意路径
（`vendor/sifli` 的 SF32LB52 移植就是这么做的）。因此本移植的全部代码都在参赛
仓内，**`nuttx/`、`packages/`、`vendor/` 等公共仓零改动**，符合参赛要求。

manifest 中的 `<linkfile>` 把本目录软链到
`vendor/openvela/boards/contest2026_252_board`，两个 CONFIG 路径都指向那里。

## 二、目录结构

```
board/contest_board/
├── chip/                       # BK7258 芯片层（→ CONFIG_ARCH_CHIP_CUSTOM_DIR）
│   ├── bk7258_memorymap.h      # 内存映射与外设基址
│   ├── bk7258_uart.h           # UART 寄存器定义
│   ├── bk7258_gpio.[ch]        # GPIO / 引脚复用
│   ├── bk7258_clockconfig.[ch] # 外设时钟门控与时钟源选择
│   ├── bk7258_start.c          # 复位入口：VTOR/FPU/.data/.bss → nx_start()
│   ├── bk7258_irq.c            # NVIC 中断控制
│   ├── bk7258_timerisr.c       # SysTick 系统节拍
│   ├── bk7258_serial.c         # UART 字符设备驱动
│   ├── bk7258_lowputc.c        # 早期调试输出
│   ├── bk7258_allocateheap.c
│   ├── include/irq.h           # 60 个外设中断号
│   └── Kconfig / CMakeLists.txt / Make.defs
├── src/                        # 板级初始化
├── include/board.h             # 时钟与引脚约定
├── scripts/ld.script           # 链接脚本（含 XIP 地址推导）
├── configs/nsh/defconfig       # 最小 NSH 基线
└── tools/
    ├── bk_crc_pack.py          # flash CRC 编码 / 校验
    └── bk_flash.py             # 持续等待复位窗口的烧录器
```

## 三、关键硬件事实（来自 BK7258 Datasheet V2.1 与 Beken bk_idk SDK）

| 项目 | 值 |
| --- | --- |
| CPU | Armv8-M STAR-MC1（Cortex-M33 兼容），最高 480MHz |
| SRAM | 640KB 共享 SRAM @ `0x28000000`（数据视图） |
| Flash | XIP 窗口 @ `0x02000000` |
| PSRAM | `0x60000000` |
| UART0 | `0x44820000`，IRQ 4 |
| UART1 / UART2 | `0x45830000` IRQ 15 / `0x45840000` IRQ 16 |
| 外设中断数 | 60（`INT_ID_MAX`） |
| UART 时钟 | XTAL 26MHz，`baud = 26MHz / (clk_div + 1)` |
| 控制台引脚 | GPIO10 = RX，GPIO11 = TX（同时是 ROM 下载口 DL_UART） |

CPU0 以 Secure（`CONFIG_SPE=1`）运行，故地址偏移为 0，直接使用上述基址。

> 注意：Beken SDK 内部 `GPIO_DEV_UARTn` 的编号比 `UARTn_xx_PIN` 大 1，即硬件
> 文档的 UART0 在 SDK 枚举里叫 UART1。本移植统一采用 **datasheet 的编号**。

## 四、Flash 布局

BK7258 的 flash 控制器在 XIP 取指时做 CRC 校验，**每 32 字节数据后插入 2 字节
CRC**，因此物理地址 = 虚拟地址 × 34/32。由分区表
（`middleware/boards/bk7258/partitions.csv`）：

| 分区 | 物理偏移 | 物理大小 | 虚拟（CPU 可见） |
| --- | --- | --- | --- |
| bootloader | `0x0` | 68KB | — |
| app | `0x11000` | 1836KB | `0x10000`，即 XIP 地址 **`0x02010000`**，1728KB |

链接脚本据此把 `flash` 段起点设为 `0x02010000`。

`tools/bk_crc_pack.py` 实现该 CRC 编码。算法参数（MSB-first、多项式 `0x8005`、
初值 `0xFFFF`、大端存储）是对 Beken SDK 中随附的已编码镜像做参数穷举得到的，
并在 3 个样本共 3868 个块上逐块复算，**零失配**。

## 五、编译

在 openvela 工作区根目录（本仓上一级）：

```bash
./build.sh vendor/openvela/boards/contest2026_252_board/configs/nsh --cmake -j8
```

macOS（Apple Silicon）可用仓库根的封装脚本：

```bash
./build-macos.sh vendor/openvela/boards/contest2026_252_board/configs/nsh
```

产物在 `cmake_out/contest2026_252_board_nsh/`：`nuttx`（ELF）、`nuttx.bin`。

编译后镜像布局（已验证）：

```
_vectors  0x02010000    ← 向量表位于镜像首字节
__start   0x02010130
初始 SP   0x28002464    ← _ebss + CONFIG_IDLETHREAD_STACKSIZE
.data     0x28000000
```

## 六、生成可烧录镜像

```bash
python3 board/contest_board/tools/bk_crc_pack.py \
    cmake_out/contest2026_252_board_nsh/nuttx.bin nuttx_crc.bin
```

`nuttx_crc.bin` 写入 flash **物理偏移 `0x11000`**（app 分区），保持 bootloader
分区不动。

自检：

```bash
python3 board/contest_board/tools/bk_crc_pack.py --verify nuttx_crc.bin
```

## 七、串口

开发套件 R1 板载 CH340（VID `0x1A86` / PID `0x7523`）接到 UART0，
即 GPIO10/GPIO11。控制台参数 **115200 8N1**。

macOS 下设备节点形如 `/dev/cu.usbserial-xxx`。

## 八、烧录：这块板子的实测坑

以下都是在真机上验证过的结论，直接照搬厂商文档会踩坑。

### 8.1 flash 实际型号

bootrom 报告 flash MID `0x1765c8`：厂商 `0xc8`（GigaDevice），容量位 `0x17`
即 2^23 = **8MB**，与 datasheet 中 QFN88 封装的 8MB SiP flash 一致。

### 8.2 `bk_loader` 的自动复位在本板无效

`bk_loader` 的 `--reset_type 0`（DTR/RTS）在本板上**不能复位芯片**，表现为一直卡在

```
Please reset the chip
Waiting reset......
Get bus failed
```

而且 `--retrycnt` 并不会延长这个 10 秒窗口。声网文档对此有过提示——"当烧录工具
无法自动重启开发板时，可以手动重启"。

原因是本板的 CH340 控制线**没有接到芯片的 CEN（复位）脚**，工具无法自动复位；
唯一的复位途径是手按板子右侧的 RST 键。而 `bk_loader` 的等待窗口固定约 10 秒，
`--retrycnt` 也不会延长它——于是烧录变成了"盲按 RST 去撞那 10 秒"的运气游戏。
实测用 for 循环反复启动 `bk_loader` 时，连续 35 轮没命中过。

本目录的 `tools/bk_flash.py` 解决了这个问题：它**不设超时**，持续发
`CMD_LinkCheck` 直到芯片应答，因此**任意一次复位都会被捕获**，不需要抢时机。

```bash
# 烧录（地址为物理偏移，十六进制）
python3 tools/bk_flash.py nuttx_crc.bin 0x11000

# 从备份恢复出厂 app 分区
python3 -c "d=open('bk7258_backup/factory_full.bin','rb').read(); \
    open('/tmp/restore.bin','wb').write(d[0x11000:0x47000])"
python3 tools/bk_flash.py /tmp/restore.bin 0x11000
```

跑起来后按一次 RST 即可，脚本会自动完成握手、擦写和回读校验。

> **务必先备份再烧录。** 烧入不能启动的镜像后，板子不再提供任何串口响应
> （出厂 app 自带的下载响应器随之消失），只能靠上面的持续等待重新接管。本次
> 调试中出现过这种情况，就是用这个脚本恢复的。

### 8.3 bootrom 的链接波特率

一个容易误判的点：在 **115200** 下对 bootrom 发 `CMD_LinkCheck`
（`01 e0 fc 01 00`）会收到看似合法的 4 字节 `04 0e 01 00`，很容易被当成"协议不
兼容的精简帧"。实际在 **1500000** 下收到的是完整帧：

```
04 0e 05 01 e0 fc 01 00
└preamble┘ └─echo──┘ └code=0x01, status=0x00
```

这正是标准 BK72xx BootROM 帧格式，115200 下那个短帧只是波特率不匹配的误读。

因此用社区工具 `bk7231tools` 时，**link_baudrate 必须一并设成 1500000**——它的
CLI 把链接波特率写死为 115200，只能走 API：

```python
from bk7231tools.serial import BK7231Serial
s = BK7231Serial(port="/dev/cu.usbserial-310",
                 baudrate=1500000, link_baudrate=1500000)
s.hw_reset()
s.connect()          # protocol: FULL, flash size: 8 MB
```

### 8.4 CRC 算法的真机交叉验证

从芯片读回的 flash 原始字节是 **CRC 编码后的物理数据**（`bk_loader read` 不做解
码）。用 `tools/bk_crc_pack.py --verify` 校验读回的 bootloader 区，120 个块**零失
配**——这在真实硬件上再次确认了第四节中逆向得到的 CRC 参数。

同时也说明：写入时喂给烧录工具的必须是 **CRC 编码后**的镜像（即 `nuttx_crc.bin`），
地址用**物理**偏移 `0x11000`。

## 九、当前状态

- [x] 芯片层：启动、中断、时钟、GPIO、UART、SysTick、堆
- [x] 板级层：defconfig、链接脚本、板级初始化
- [x] 构建集成：openvela CMake 构建通过，干净重建可复现
- [x] 镜像布局：`_vectors` 位于镜像首字节，SP / 复位地址正确
- [x] flash CRC 打包：对厂商样本与真机读回数据双向验证，零失配
- [x] 出厂固件备份与恢复：完整 8MB，两次独立读取互证，恢复后 Demo 正常启动
- [x] 真机烧录：写入 `0x11000` 成功，回读逐字节一致
- [ ] 真机启动：**未通过**

### bootloader 行为：两条实测确证

用最小差异法在真机上测出来的，都是单变量实验：

| 实验 | 改动量 | 结果 | 结论 |
| --- | --- | --- | --- |
| 出厂镜像里翻转一个日志字符串的字母 | 1 字节 | 正常启动 | **bootloader 不校验 app 镜像** |
| 出厂镜像只改向量表第 2 个字 | 4 字节 | 不启动 | **bootloader 使用复位向量进入 app** |

第一条印证了官方文档（download 分区为全 `0xFF` 时直接跳转、不校验），也**否定了
"需要为镜像补 CRC/hash 头"的猜测**——`img hash err!` 等字符串属于 OTA 路径。
第二条确认入口机制就是标准的"读向量表取 SP 与复位地址"，本移植的镜像布局没有问题。

### 硬件事实（来自板级原理图，AIDK AI 玩具开发板 / Agora Device Kit R1）

| 项 | 依据 |
| --- | --- |
| UART0 = GPIO10/GPIO11 | Sheet 2 引脚 51 `P10/DL_0RX`、52 `P11/DL_0TX` → 网络 `RX0`/`TX0` |
| CH340E 接 UART0 | Sheet 3：USB2 → U12 CH340E → RXD/TXD → `TX0`/`RX0`（经 100R） |
| **CH340 的 RTS#/CTS# 悬空** | Sheet 3 上这两脚标注未连接 |
| RST 键直连 CEN | Sheet 2：K1(RESET) → `CEN`，R2 10K 上拉到 VIO |
| 马达在 GPIO9 | Sheet 4：网络 `P9` → R51 1K → Q2 MMBT3904 → CN10 马达 |
| MCU 控制的 LED | Sheet 4：网络 `LED1`→R63 1K→LED3 红、`LED2`→R64 330R→LED4 绿 |
| 充电状态灯**与 MCU 无关** | Sheet 3：LED1/LED2 由充电 IC ETA4322 的 `CHRG`/`FULL` 驱动 |

第三条是烧录困难的物理根因：**没有任何信号能驱动 CEN**，所以厂商工具的
`--reset_type 0/3`（DTR/RTS）在本板必然无效，只能手按 RST。`tools/bk_flash.py`
就是为此而写。

最后一条曾误导过调试：板子插着 USB 时左下角绿灯常亮，那是充电指示，不能用来
判断固件是否运行。

### 真机实验记录

全部为单变量对照，基准是"出厂 app 镜像原样可启动"。

| # | 改动 | 结果 | 可推出的结论 |
| --- | --- | --- | --- |
| 1 | 翻转一个日志字符串的字母（1 字节，非代码） | ✅ 启动 | **bootloader 不校验 app 镜像内容** |
| 2 | 只改向量表第 2 个字（4 字节） | ❌ 不启动 | **改复位向量会导致不启动** |
| 3 | 向量改指 `0x200` + 该处放桩 | ❌ 无反应 | 作废，见下 |
| 4 | 向量改指 `0x20000` + 该处放桩 | ❌ 无反应 | 作废，见下 |
| 5 | 向量不动，桩覆盖出厂入口 `0xCAFA0` | ❌ 无反应 | 作废，见下 |

实验 1 是最有价值的一条：它**否定了"需要逆向 bootloader、为镜像补 CRC/hash 头"
的整个方向**，与官方文档一致（download 分区为全 `0xFF` 时直接跳转、不校验）。
bootloader 二进制里的 `img hash err!`、`head CRC32 fail.` 属于 OTA 升级路径。

### 探针设计上踩过的坑

实验 3/4/5 的"无反应"**不能作为证据**，因为探针本身有缺陷。四次教训的共同点都是
**在未经验证的前提上建结论**：

1. **依赖 `SYSRESETREQ`。** 前两版探针靠写 `AIRCR` 自复位来证明"代码在跑"。但该
   请求是否真能重启本 SoC 从未验证——若被屏蔽，桩执行了也不会复位，现象与"未执行"
   完全一致。
2. **LED 引脚靠推断。** GPIO40/41 是从原理图网络标号的排列顺序猜的，文本里无法确证。
3. **负载没有供电。** 马达与 LCD、SD NAND 共用 `LDO_3V3`，而该 LDO 的使能脚是
   **GPIO52（网络 `LDO33_EN`，引脚 6 = P52）**。不先拉高它，马达根本没电——前三版
   马达探针全部无效。
4. **误认指示灯。** 板子插 USB 时左下角常亮的绿灯是充电 IC `ETA4322` 驱动的充电
   状态灯（Sheet 3），与 MCU 无关；MCU 控制的是 Sheet 4 的 LED3/LED4，且它们挂在
   "外扩焊点"上，**可能未贴件**。

有效的探针必须满足：不依赖时钟门控、引脚复用、UART、复位机制，且驱动的负载确实
通电。`CONFIG_BK7258_LED_PROBE` 的最终版本先拉高 GPIO52 给 LDO 上电，再驱动
GPIO9 的马达——这是本目录里唯一一个前提都被核实过的探针。

