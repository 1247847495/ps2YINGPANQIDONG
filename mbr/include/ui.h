#ifndef _UI_H_
#define _UI_H_

#include "config.h"

// 显示中文引导菜单, 返回用户选择(或超时默认)对应的按键类型
// 菜单: 文件管理器 / OPL 游戏列表 / 启动FMCB菜单 / 返回记忆卡(PS2浏览器) / 读取DVD / 设置
//   - 返回记忆卡: ExecOSD(BootBrowser) 直接跳转, 不返回
//   - 读取DVD: startGameDisc() 启动光盘游戏, 不返回
//   - 设置: 浏览硬盘选择三个菜单的启动 ELF、调节倒计时,
//     保存后自动生成 hdd0:__sysconf:/osdmenu/OSDMBR.CNF
TriggerType uiMenuLoop(void);

#endif
