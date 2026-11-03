// TraceBridge 窗口（骨架）：页签初始化与登录校验

import { initTabs } from "./lib/tabs.js";
import { initWindow } from "./window-common.js";

initTabs();

void initWindow().catch(() => {});
