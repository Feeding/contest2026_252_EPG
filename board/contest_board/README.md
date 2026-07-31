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

### 真机实验记录

基准是"出厂 app 镜像原样可启动"，全部单变量对照。

| # | 改动 | 结果 |
| --- | --- | --- |
| 1 | 翻转一个日志字符串的字母（1 字节，虚拟偏移 `0x698`） | 正常启动 |
| 2 | 只改向量表第 2 个字（4 字节） | 不启动 |
| 3 | 向量改指 `0x200` + 该处放桩 | 桩不执行 |
| 4 | 向量改指 `0x20000` + 该处放桩 | 桩不执行 |
| 5 | 向量不动，桩覆盖出厂入口 `0xCAFA0` | 桩不执行 |
| 6 | 保留前 4KB，其余全填 NOP，末尾放桩 | 桩不执行 |

实验 1、2 与下节反汇编结论一致（不校验、走复位向量）。**但实验 1 当时未回读验证
那个字节是否真的写进 flash，所以它只能作为旁证，不能独立支撑结论**——真正的依据
是反汇编。

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

### bootloader 启动逻辑（反汇编所得，非推断）

把备份里的 bootloader 分区 CRC 解码后反汇编（`objdump -b binary -m armv8-m.main
-M force-thumb --adjust-vma=0x02000000`），启动路径完全清楚：

`0x02001790` —— 计算 app 地址并跳转：

```
ldr  r0, ="app"          ; 分区名
bl   0x02002194          ; 按名字查分区表
movs r3, #34
ldr  r0, [r0, #52]       ; partition->offset（物理）
lsls r1, r0, #5          ; ×32
sdiv r2, r1, r3          ; ÷34        <- 物理→虚拟换算
add  r0, r2, #0x2000000  ; + XIP 基址
bl   0x0200172c          ; 跳转
```

**这条路径上没有任何校验**：查分区、算地址、直接跳。

`0x0200172c` —— 跳转本体：

```
ldrd r5, r6, [r4]        ; r5 = vector[0] (SP), r6 = vector[1] (PC)
bl   0x02000844
bl   0x02001890          ; 关闭并无效化 cache（见下）
str  r4, [0xE000ED00+8]  ; VTOR = app 基址
msr  MSP, r5
bx   r6                  ; 跳到 vector[1]
```

`0x02001890`（r0=0）操作的是 SCB（`0xE000ED00`）：清 `CCR` 的 bit16（数据 cache
使能），再按 set/way 循环无效化。**即 app 是在 cache 关闭的状态下开始执行的。**

由此得到三条确定结论：

1. **bootloader 不校验 app 镜像**——反汇编实锤，与官方文档一致。
2. **入口机制是标准的**：读向量表取 SP/PC，设 VTOR，跳 `vector[1]`。
3. **app 的 XIP 基址 = `0x02000000 + 物理偏移 × 32 ÷ 34`**。代入本板 app 分区偏移
   `0x11000` 得 **`0x02010000`**——独立验证了 `scripts/ld.script` 里的地址。

第 3 条同时解释了 flash 的 34:32 编码为何贯穿始终：bootloader 自己就在做这个换算。


### 探针设计的复盘

上表中 3~6 的"桩不执行"**都不能作为证据**，各版探针都有缺陷。共同点是**在未经
验证的前提上建结论**：

1. **依赖 `SYSRESETREQ`。** 前两版靠写 `AIRCR` 自复位证明"代码在跑"，但该请求是否
   真能重启本 SoC 从未验证；若被屏蔽，桩执行了也不会复位，现象与"未执行"一致。
2. **LED 引脚靠推断。** GPIO40/41 是从原理图网络标号的排列顺序猜的。
3. **负载没有供电。** 马达与 LCD、SD NAND 共用 `LDO_3V3`，其使能脚是 **GPIO52**
   （网络 `LDO33_EN`，引脚 6 = P52）。不先拉高它，马达根本没电。
4. **误认指示灯。** 插 USB 时常亮的绿灯由充电 IC `ETA4322` 驱动，与 MCU 无关。
5. **GPIO9 可能被占用。** datasheet 里 GPIO9 的复用之一是 `32K_XI`，原理图上
   32.768kHz 晶振也画在 P8/P9 附近。
6. **NOP 滑梯撞上 cache 关闭。** bootloader 跳转前会关掉 cache，从 `0xCAFA0` 滑到
   镜像末尾需约 47 万条 NOP，在未开 cache 的 XIP flash 上极慢，几乎必然在到达桩
   之前被看门狗复位。

**目前最可靠的探针**是 GPIO11 位翻转：GPIO11 = UART0 TX → CH340（datasheet 与
原理图双重确认），把它当普通 GPIO 翻转，CH340 会把电平跳变解成乱码字节，读到任何
数据即证明代码在执行。不依赖时钟门控、引脚复用、UART 外设、复位机制或供电，也不
需要肉眼判断。安全别名与非安全别名两版都试过，均无输出。

**写入链路已被独立验证**，不是问题所在：桩所在区域回读后 32 字节逐字节在位，其
虚拟地址算出来正是 `0xCAFA0`（复位向量所指），该区域 240 个 CRC 块零失配；CRC
编解码另做过离线 round-trip，32KB 与 832KB 两个跨度均与原始物理字节逐字节一致。

### 尚未解决

反汇编已证明 bootloader 无条件跳转、地址算法与本移植一致、入口机制标准，写入链路
也已验证，但放在入口处的裸机桩仍不执行。下一步建议从跳转前的 `0x02000844` 入手
（它读一个标志位后条件调用 `0x02000a90`，再尾调 `0x02000ac2`），确认它是否关闭了
UART 或改变了外设访问权限——若是，则本移植早期访问 UART/GPIO 时会立即故障，而出厂
app 因为入口处先做完整初始化而不受影响。

### 下一步：用 SWD 在跳转处断下（需调试器）

黑盒探针已经走到尽头——bootloader 的行为完全查清且与本移植吻合，但合规镜像仍不
执行。要再进一步，最有效的是在跳转指令处断下，直接观察跳转后 PC 落在哪、是否触发
fault。

**接线**（引脚来自本板原理图 Sheet 2 与 BK7258 datasheet）：

| 信号 | BK7258 引脚 | 说明 |
| --- | --- | --- |
| SWCLK | 83（`P20/0SCL/SWCLK/R6/D9`） | 板上复用为 `IIC1_SCL`，接 G-Sensor |
| SWDIO | 84（`P21/0SDA/SWDIO/ADC6/R5/D8`） | 板上复用为 `IIC1_SDA` |
| GND | 任意地 | — |

注意这两脚在本板上被 G-Sensor 的 I2C 占用，需确认有测试点或焊盘可接；引脚 43 另有
独立的 `SWD` 信号，用途需查 datasheet 确认。

**软件**：`brew install open-ocd`。BK7258 是 Armv8-M（STAR-MC1，Cortex-M33 兼容），
可先用通用 Cortex-M 配置起步：

```bash
openocd -f interface/cmsis-dap.cfg -c "transport select swd" \
        -f target/swj-dp.tcl -c "adapter speed 1000" \
        -c "swj_newdap bk7258 cpu -irlen 4; dap create bk7258.dap -chain-position bk7258.cpu" \
        -c "target create bk7258.cpu cortex_m -dap bk7258.dap" -c init
```

**断点位置**（地址来自本目录记录的反汇编）：

| 地址 | 含义 |
| --- | --- |
| `0x02001790` | 计算 app 地址的函数入口 |
| `0x0200172c` | 跳转函数入口，`r0` = app 基址（应为 `0x02010000`） |
| `0x02001738` | 刚读完 `vector[0]/[1]`，此时 `r5`=SP、`r6`=PC |
| **`0x02001782`** | **`bx r9`——跳转本身，在此单步进入即可看到目标处第一条指令** |

要确认的三件事：

1. `0x0200172c` 处 `r0` 是否等于 `0x02010000`（若不是，说明分区表解析与预期不符）
2. `0x02001738` 之后 `r5`/`r6` 是否等于我们镜像的 `vector[0]`/`vector[1]`
3. 在 `0x02001782` 单步后 PC 落在哪——若立即进入 HardFault/BusFault 处理，读
   `CFSR`(`0xE000ED28`)、`HFSR`(`0xE000ED2C`)、`BFAR`(`0xE000ED38`) 即可定位故障源

若 SWD 连不上，先确认 eFuse 是否禁用了调试口（本板出厂固件是量产固件，有此可能）。
