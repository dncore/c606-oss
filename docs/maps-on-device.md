# 给设备装地图

固件读取 `MAP/*.map`（Mapsforge 二进制格式，**v3/v4/v5 都支持**），
目录在 eMMC 根下，和厂商留下的 `map.trans` 放一起。没有 `MAP/` 目录时地图页
只显示 “Waiting for a GPS fix / No maps in /sdcard/MAP”。

## 地图从哪来

官方 Mapsforge 下载站按国家/省切分：

```
https://download.mapsforge.org/maps/v5/asia/china/
```

例如山东（含青岛）是 `shandong.map`，约 **85 MB**；单省文件对 7 GB 的 eMMC
完全没压力。也可以自己用 `mapsforge-map-writer` + `osmosis` 从 OSM extract
生成更小的城市图。

## 怎么传进去

### 推荐：USB 存储模式（大文件，秒级）

固件带 `usbmsc` 控制台命令（`main/devcon.c`）：

```sh
# 串口发命令，N 秒后设备自动重启回正常模式
echo "usbmsc 180" | sudo tee /dev/ttyACM0

# 主机侧：等 U 盘出现（7.1G vfat），挂载并拷贝
sudo mount -o rw /dev/sda /mnt/c606
sudo mkdir -p /mnt/c606/MAP
sudo cp shandong.map /mnt/c606/MAP/
sync && sudo umount /mnt/c606
```

85 MB 实测约 **7 秒**。设备在超时后自己 `esp_restart`，USB-Serial-JTAG 控制台
随之恢复；不想等就长按三键复位。

### 小文件：`put` 命令

GPX、单个小地图（几 MB）可以直接走控制台：

```sh
# devcon: put <local> <remote>
tools/devcon.py /dev/ttyACM0 put qingdao.map /sdcard/MAP/qingdao.map
```

`put` 是 hex + 逐行 ACK，大约 100 KB/s，只适合小文件。

## 验证

开机日志会打印每个地图的版本、bbox 和 way tag 数：

```
mapfile: /sdcard/MAP/shandong.map: v5, bbox 34275750,114713300..38540000,123663200,
         288 way tags, 3 intervals (base 5/10/14)
mapview: map 0: shandong.map
```

控制台：

```
ls /sdcard/MAP          # 文件在不在
pos 36.0671 120.3826    # 把地图中心设到青岛市南区（不需要 GPS 定位）
zoom 12
```

`pos` 只影响地图页；它会被骑行开始/复位清掉，需要时重新发。

## 已知性能特征

- 单次整屏地图渲染（山东图、z12、约 2400 条 way）在设备上约 **235 ms**，
  这是**后台 mapview 任务**的耗时，不阻塞 UI（`refresh_cb` 已改为 try-lock）。
- 因此地图页的平移/缩放大约 4 fps 更新；数据页不受影响。
- 首次进入地图页的 LVGL 帧时间约 84 ms，其余页面约 9–13 ms。

## 授权

Mapsforge 官方下载站的地图由 OSM 数据生成，使用需遵守
[ODbL](https://opendatacommons.org/licenses/odbl/)；随固件分发时保留来源说明。
