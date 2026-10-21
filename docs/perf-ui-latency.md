# C606 UI 响应性优化记录

> 目标：让界面"跟手"。本文记录**怎么测、测出了什么、改了什么、每项改动的收益**，
> 以及还没解决的问题。适用于 `cc700pro` 构建（C606 V2 硬件）。
>
> 分支：`perf/ui-latency`。相关 commit：`040aa86`（测量）、`a4ade27`（优化）。

## 1. 先把"跟手"定义成可测的量

"跟手"不是 fps，而是**从手指按下到屏幕出现变化**的时间。把它拆成四段：

```
按下
 ├─ 触摸采样延迟      采样周期 / 2（平均）
 ├─ 手势判定延迟      手指要移动 X px 才认作翻页；X / 滑动速度
 ├─ 渲染延迟          lv_timer_handler 里排版 + 软件光栅化
 └─ 刷屏延迟          flush_cb → DMA 完成（i80 总线传输）
```

对应四个计数器（`main/ui_port.c`）：

| 计数器 | 起点 | 终点 | 含义 |
|---|---|---|---|
| `press_*` | 第一个 PRESSED 采样 / 注入的按键 | 该次输入的**第一块** flush DMA 完成 | 端到端"看到反应" |
| `render_*` | 进入 `lv_timer_handler()` | 返回 | 排版 + 光栅化 + 提交 flush |
| `flush_*` | `flush_cb` 入口 | DMA-done 中断 | 一块 60 行像素占用总线的时间 |
| `frames` | — | — | flush 次数（**不是**完整帧，见下） |

两个必须说清楚的细节：

1. **`frames` 是 flush 块数，不是帧数。** LVGL 以 60 行（28.8 KB）为一块渲染，
   一次全屏重绘约 6 块。所以 `fps` 更准确的名字是"块/秒"。
2. **`press` 只等到第一块。** 这正是用户感知的"有反应了"；整页画完会晚几毫秒。

## 2. 测量系统

### 2.1 固件侧

`main/ui_port.c` 里加了一组 `volatile uint32_t` 计数器和两个时间戳：

```c
static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    s_flush_t0 = esp_timer_get_time();
    ...
}

static bool IRAM_ATTR on_flush_done(...)          /* DMA 完成中断 */
{
    uint32_t dt = esp_timer_get_time() - s_flush_t0;
    s_flush_sum += dt; if (dt > s_flush_max) s_flush_max = dt;
    if (s_press_pending) {                        /* 按下 → 第一块出画 */
        uint32_t lat = esp_timer_get_time() - s_press_at;
        s_press_sum += lat; s_press_n++;
        s_press_pending = false;
    }
    lv_display_flush_ready(s_disp);
    return false;
}
```

`lvgl_task` 里量 `lv_timer_handler()` 的耗时；`touch_read_cb` 在
"未按下 → 按下"的边沿记 `s_press_at`。

`devcon` 新增命令：

```
perf            # 打印一行，例如：
perf up=8216ms frames=163 fps=19.8 render_avg=8.87 render_max=43.11 \
     flush_avg=1.10 flush_max=2.11 press_n=10 press_avg=17.2 press_max=28.3
perf reset      # 清零并重新计时
```

按键是**注入**的，不经过触摸控制器，所以 `devcon` 的 `key` 分支在调用
`s_key_cb` 前先调 `ui_port_mark_input()`，让 `press_*` 也能覆盖"按键→出画"。

### 2.2 主机侧

`tools/perf_measure.py`：

```sh
python tools/perf_measure.py /dev/ttyACM0 --mode swipe --swipes 12 --label before
python tools/perf_measure.py /dev/ttyACM0 --mode key   --swipes 12 --label after
```

- `swipe`：交替左右滑 120 ms，覆盖翻页手势路径。
- `key`：注入 key 0；空闲态不会翻页，所以测"按键翻页"要先 `key 2 1` 进入骑行态。
- 每次跑之前自动 `perf reset`，结束后读一次 `perf`。

### 2.3 测量纪律

- **重复三次**取区间，单次容易吃到地图页的尖峰。
- 测之前确认没有地图页刷新、没有正在结束的骑行。
- 串口打开方式会影响结果：本机必须在 `open()` 前把 **DTR/RTS 都置低**，
  否则 USB-Serial-JTAG 会被留在 ROM 下载模式（见第 7 节）。

## 3. 基线（优化前）

```
press avg/max = 58.4 / 108.1 ms
render avg    = 8.76 ms
flush avg     = 1.72 ms
fps           = 19.5 块/秒
```

## 4. 根因拆解

按第 1 节的预算逐项核对，四个原因按影响排序：

### 4.1 手势阈值 50 px（最大项）

LVGL 默认 `LV_INDEV_DEF_GESTURE_LIMIT = 50`。以 160 px / 120 ms 的滑动速度
（约 1.33 px/ms）计算，光"跨过 50 px"就要 **37 ms**，此时页面才刚开始切。

### 4.2 采样与刷新周期 33 ms

`LV_DEF_REFR_PERIOD = 33` 同时决定：

- 显示刷新定时器（30 Hz）
- **触摸读取定时器**（`lv_indev.c:105`：`lv_timer_create(..., LV_DEF_REFR_PERIOD, ...)`）

所以触摸最坏要等 33 ms 才被采样一次。

### 4.3 CPU 只有 160 MHz

ESP-IDF 对 ESP32-S3 的默认值是 160 MHz，而厂商固件跑 **240 MHz**
（启动日志 `cpu freq: 240000000 Hz`）。软件光栅化直接慢 1/3。

### 4.4 每帧往 PSRAM 镜像一份画面

`devcon_mirror()` 被 `flush_cb` 无条件调用，把每块脏区 memcpy 到 PSRAM 的
截图缓冲。这是给 `shot` 命令用的，正常使用时纯属浪费，还会污染 cache。

### 4.5 `-Os`

`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`。LVGL 是计算密集的软件渲染，`-Os`
在循环里牺牲了速度。

## 5. 改动与逐项验证

每一项都单独上机测过；下面是最终配置的合并效果。

### 5.1 CPU 160 → 240 MHz

```ini
# sdkconfig.defaults
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

> 注意：必须写 choice 符号 `..._240=y`，只写 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`
> 不会生效（生成的 sdkconfig 仍选 160）。

代价：功耗上升。续航未测。

### 5.2 刷新 33 → 16 ms

```ini
CONFIG_LV_DEF_REFR_PERIOD=16
```

同时把触摸采样从 33 ms 降到 16 ms。渲染仍是一块一块做，不会因为周期减半就
每帧都全屏重绘。

### 5.3 手势阈值 50 → 16 px

```ini
CONFIG_LV_INDEV_DEF_GESTURE_LIMIT=16
```

按 1.33 px/ms 算，判定时间从 ~37 ms 降到 ~12 ms。

风险：阈值太低会让"带一点横向抖动的点击"误判成翻页。16 px 在 240 px 宽的屏上
实测没有明显误触；如果出现，回调到 20–24。

### 5.4 触摸采样单独压到 8 ms

LVGL 没有公开的 setter，私有结构里有定时器句柄：

```c
#include "lvgl_private.h"        /* LVGL 9 only */

if (s_indev->read_timer)
    lv_timer_set_period(s_indev->read_timer, 8);
```

只改触摸，不动 16 ms 的渲染节奏——渲染周期再短会打满 CPU。

### 5.5 `-Os` → `-O2`

```ini
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

镜像从 1.01 MB 涨到 1.13 MB（槽位 7.8 MB，无所谓）。

副作用：`-O2` 打开了 `-Wformat-truncation`，`route_path()` 的
`snprintf("%s")` 直接编译失败。改成显式有界拷贝（`main/route.c` 的
`path_join()`），没有加 `-Wno-*` 掩盖。

### 5.6 截图镜像改为按需

```c
void devcon_mirror(...)
{
    if (!s_frame || !s_mirror) return;   /* 只有 shot 期间才为真 */
    ...
}
```

`cmd_shot()` 现在：置 `s_mirror` → 请求整屏重绘 → 等 120 ms → 在 `ui_lock`
下 memcpy 一份快照 → **关掉镜像** → 再从快照慢慢通过 USB 发出去。
这样正常帧不再有额外 memcpy，USB 传输也不再占着 UI 锁。

## 6. 结果

12 次滑动，重复三次：

| 版本 | press avg / max | 说明 |
|---|---|---|
| 基线 | **58.4 / 108.1 ms** | 160 MHz、30 Hz、阈值 50、-Os、每帧镜像 |
| +240 MHz / 60 Hz / -O2 / 按需镜像 | 42.1 / 67.7 ms | 阈值仍是 50 |
| +阈值 24 px | 37.3 / 45.8 ms | |
| +阈值 16 px + 8 ms 采样 | **19.0 / 28.2**、**26.4 / 33.1**、**21.5 / 33.4** | 三次重复 |

骑行态按键翻数据页（不含地图）：**17.2 / 28.3 ms**，`render_max` 43 ms。

装上真实地图（山东 v5）后热态重测：**30.5–31.2 / 34.6–43.5 ms**，
`render_max` 84–87 ms——地图页把平均值抬高了，见第 9 节。

结论：端到端延迟降到基线的 **1/2.7 左右**，峰值从 108 ms 降到 33 ms（不含地图）。
剩下的约 17–20 ms 基本就是 8 ms 采样 + 12 ms 手势判定 + 9 ms 渲染。

## 7. 踩过的坑

1. **`sdkconfig.defaults` 改了不生效。** 非 `c606` 板型用的是仓库根目录的
   `sdkconfig.cc700pro`（不是 `build-*/sdkconfig`）。**改 defaults 后必须删掉它重新生成**，
   否则会以为改了、其实编的是旧配置。我因此白测了一轮。
2. **DTR/RTS 置低会复位设备。** 实测：`dtr=False, rts=False` 打开串口后
   uptime 只有 ~200 ms，也就是说**每一次"热态"测量其实都是冷启动测量**；
   `dtr=True, rts=True` 打开则 uptime 连续（39 s → 43 s）。
   `perf_measure.py` 已改为置高。设备若已被留在 ROM 下载模式，用
   `esptool --before no-reset` 仍能连上，不是刷坏。
3. **`-O2` 触发 `-Werror=format-truncation`**（见 5.5）。
4. **`frames` 容易误读成帧率**（见 1）。
5. **地图页会污染最大值。** 12 次滑动里只要经过地图页，`render_max` 就是 170 ms+；
   判断数据页的手感要看 `press_avg` 和按键那组。

## 8. 复现

```sh
cd c606-oss
export HOME=$PWD/../.buildhome XDG_CACHE_HOME=$PWD/../.buildhome/.cache
export IDF_TOOLS_PATH=$PWD/../.idf-tools
. ../esp-idf/export.sh

rm -f sdkconfig.cc700pro                 # 关键：让 defaults 重新生效
idf.py -B build-opt -DBOARD=cc700pro build
sudo ../.venv/bin/python ../flash_c606.py --image build-opt/cc700pro_oss.bin --confirm

P=$(ls /dev/ttyACM* | head -1)
sudo ../.venv/bin/python tools/perf_measure.py "$P" --mode swipe --swipes 12
sudo ../.venv/bin/python tools/perf_measure.py "$P" --mode key   --swipes 12   # 先 key 2 1 进骑行态
```

回滚：`flash_c606.py --restore --confirm`（整片 16 MB 原厂备份）。

## 9. 地图页尖峰：已定位并修复

现象：一进入地图页，`render_max` 就出现 177–230 ms，基线也有 191 ms。

根因不是渲染，而是**锁**：

- `mapview.c` 的 `render_task` 在**整段瓦片渲染期间**持有 `s_mtx`；
  实测一次山东省图 z12 全屏渲染 **235 ms**（地图页标题里的 `235 ms`）。
- `refresh_cb()` 是 LVGL 定时器，用 `portMAX_DELAY` 拿同一把 `s_mtx` →
  LVGL 任务在 `lv_timer_handler()` 里被阻塞同样的时长，正好对上 177–230 ms。

修复：`refresh_cb` 改成 `xSemaphoreTake(s_mtx, 0)`，拿不到就跳过这一 tick。

验证（地图在滑动环里，连续改 4 次中心触发重绘）：

```
修复前 render_max 177–230 ms
修复后 render_max  31 ms（重绘期间）
修复后 进地图页    84–87 ms（canvas 首帧）
```

仍未完全消除的是 84–87 ms 的 canvas 首帧：地图页画布约 240×280 RGB565，
LVGL 需要把整块 blit 到绘制缓冲。下一步可以试：预渲染一帧再切换、
把 canvas 拆成横条 invalidate、或确认 LVGL 的 RGB565 图像是否走了 memcpy 快路径。

另外一个独立的性能事实：**地图渲染本身 235 ms/帧（约 4 fps）**，
所以地图页的平移/缩放仍然是"每秒几帧"的体验；这不是 UI 卡顿（数据页不受影响），
要提升得优化 `render_map()` 的 way 裁剪或瓦片读取。

## 10. 还可以做的

- 给触摸按下加即时视觉反馈（按下的格子/按钮先高亮），进一步降低"没反应"的感知。
- `LV_DRAW_SW_DRAW_UNIT_CNT > 1`（需要 LVGL 的 OS 支持）——收益与风险都待评估。
- 把 `perf` 计数做成可开关（编译期），避免长期占用几个字节和 ISR 里的两次计时。
- 240 MHz 的续航影响：骑 30 分钟对比掉电百分比。
