#pragma once

namespace wm::app
{
    /// 单实例：创建（或打开）命名互斥量。返回 true 表示本进程是第一个实例，
    /// 应当正常启动；返回 false 表示已有实例在跑，调用方应通知它并退出。
    /// 互斥量句柄在进程存续期间保持打开，进程退出时由系统释放。
    bool AcquireSingleInstance();

    /// 向已运行实例的托盘消息窗口投递"显示主窗口"消息。
    /// 托盘窗口在主窗口 Loaded 后才创建，所以这里会短暂重试；
    /// 找不到时静默返回（调用方照常退出即可，先启动的窗口马上就会出现）。
    void SignalExistingInstance();
}
