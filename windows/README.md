# Windows 文件合成

`synthesizeToFile` 在 WinRT 和 SAPI 后端均输出 WAV，使用调用时的语音、音量、音调和语速设置，在后台执行，不播放音频。文件合成使用独立合成器；`stop()` 只控制播放，不取消文件合成。

```dart
await flutterTts.awaitSynthCompletion(true);
final result = await flutterTts.synthesizeToFile(
  '你好，Windows！',
  r'C:\Users\your-name\Documents\tts.wav',
  true,
);
// result == 1 时文件已写入并关闭；失败为 0。
```

`isFullPath: true` 要求绝对路径；默认 `false` 要求相对路径，以进程当前工作目录为基准。父目录必须已存在，同名文件会被覆盖。建议使用 `.wav` 扩展名。

默认不等待写入完成，返回 `1` 仅表示任务已接受；通过 `setStartHandler`、`setCompletionHandler` 和 `setErrorHandler` 接收合成事件（这些回调也用于播放事件）。每次调用会保留当时的等待设置。合成期间再次调用返回 `0`，不影响正在执行的任务。

## Windows 验证

在安装了 Windows 桌面开发工具链和系统语音的机器上，在 `example` 目录运行 `flutter run -d windows`，通过示例或调用上述接口验证以下场景。修改等待设置和回调后，需保证前一个任务已结束。

| 场景 | 预期 |
| --- | --- |
| 等待完成，使用已存在目录下的绝对路径 | 返回 `1`，收到 start/complete 各一次；文件以 `RIFF` 开头、偏移 8 为 `WAVE`，且能播放 |
| 使用中文文件名、中文正文及 `& < >` | 正常输出，正文不会作为 SAPI XML 解析 |
| 默认相对路径，同名文件再次合成 | 文件位于当前工作目录，旧文件被覆盖 |
| 关闭等待，合成长文本 | 先返回 `1`，完成后才收到 complete，窗口保持响应 |
| 合成期间再次请求文件合成 | 第二次返回 `0`，第一份文件正常完成 |
| 文件合成期间修改语音设置或调用 speak | 文件继续使用原设置，播放正常 |
| 父目录不存在或文件不可写 | 收到 error，不收到 complete；等待模式返回 `0`，后续有效请求可成功 |
| 空文本、空文件名、路径与 isFullPath 不匹配 | 返回 `0`，不开始合成 |
| 合成期间切换 awaitSynthCompletion | 正在执行的调用仍按原等待设置返回 |
| 合成期间关闭应用 | 不通过已销毁插件发送回调 |

仓库通过 `WINAPI_FAMILY` 条件编译选择 WinRT 或 SAPI；两个分支均需在对应 Windows 构建配置下验证。macOS 上的 Dart 分析无法替代 Windows 原生编译和音频验证。
