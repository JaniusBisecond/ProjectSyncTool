#pragma once

// 命令行批处理模式：不创建窗口，用与 GUI 完全相同的核心逻辑扫描 / 比对，
// 把结果写进 logs\scan-report-<时间戳>.txt。用于自动化与结果对照。
namespace CliRunner
{
// 识别到批处理参数时执行并返回进程退出码；否则返回 -1（调用方继续走 GUI）
int RunIfRequested();
}
