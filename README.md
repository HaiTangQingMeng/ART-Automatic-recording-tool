# VHF 音频录制程序 (ART - Automatic Recording Tool)

一个基于 C++ / Win32 的 VHF 无线电音频自动录制工具，用于持续录制 VHF 频段音频并保存为文件。

## 特性

- 纯 C++ 实现，仅依赖 Windows 系统库（COM / Win32 API）
- 由 `build.bat` 调用 MSVC 工具链一键编译，无需第三方依赖
- 图形界面（Windows 子系统），适合长时间后台录制

## 构建

需要安装 **Visual Studio Build Tools**（含「使用 C++ 的桌面开发」工作负载）。

双击运行 `build.bat` 即可，脚本会通过 `vswhere` 自动定位 VS 工具链并编译生成 `VHFRecorder.exe`：

```bat
build.bat
```

等价于手动执行：

```bat
cl /EHsc /O2 /utf-8 /W3 main.cpp /Fe:VHFRecorder.exe ^
   ole32.lib uuid.lib comctl32.lib shell32.lib user32.lib gdi32.lib ^
   /link /SUBSYSTEM:WINDOWS
```

链接的系统库说明：

- `ole32.lib` / `uuid.lib` — COM 初始化与音频设备枚举
- `comctl32.lib` — 通用控件
- `shell32.lib` / `user32.lib` / `gdi32.lib` — 窗口、消息与绘图

## 使用

1. 运行编译好的 `VHFRecorder.exe`
2. 在界面中选择音频输入设备并开始录制
3. 录制的音频会按配置持续保存到本地文件

## 项目结构

```
VHF音频录制程序/
├── main.cpp      # 程序主源代码
├── build.bat    # MSVC 一键构建脚本
└── README.md
```

## 许可证

仅供个人 / 非商业学习与使用。
