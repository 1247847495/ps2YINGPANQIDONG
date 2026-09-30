#include "config.h"
#include "common.h"
#include "defaults.h"
#include "dprintf.h"
#include "hdd.h"
#include <ctype.h>
#include <errno.h>
#include <osd_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <fileio.h>

Settings settings;

int loadConfig(void);
void initConfig(void);

char configPath[] = "pfs0:" HOSDMBR_CONF_PATH;
char xfromConfigPath[] = "xfrom:" HOSDMBR_CONF_PATH;

// anyi: 无配置文件时的内置默认启动路径
static launchPath *findTrigger(TriggerType t) {
  launchPath *p = settings.paths;
  while (p) {
    if (p->trigger == t)
      return p;
    p = p->next;
  }
  return NULL;
}

static launchPath *appendTrigger(TriggerType t) {
  launchPath *np = malloc(sizeof(launchPath));
  if (!np)
    return NULL;
  np->trigger = t;
  np->paths = NULL;
  np->args = NULL;
  np->argCount = 0;
  np->next = NULL;

  launchPath **pp = &settings.paths;
  while (*pp)
    pp = &(*pp)->next;
  *pp = np;
  return np;
}

// 确保 AUTO(文件管理器) / START(OPL) / SELECT(FMCB菜单) 至少各有一条路径, 用户配置优先
void ensureDefaultPaths(void) {
  if (!findTrigger(TRIGGER_TYPE_AUTO)) {
    launchPath *p = appendTrigger(TRIGGER_TYPE_AUTO);
    if (p)
      p->paths = addStr(p->paths, "hdd0:__sysconf:pfs:/FMCB/BOOT.ELF");
  }
  if (!findTrigger(TRIGGER_TYPE_START)) {
    launchPath *p = appendTrigger(TRIGGER_TYPE_START);
    if (p) {
      p->paths = addStr(p->paths, "hdd0:__sysconf:pfs:/FMCB/BOOT1.ELF");
      p->paths = addStr(p->paths, "hdd0:__boot:pfs:/BOOT.ELF");
    }
  }
  if (!findTrigger(TRIGGER_TYPE_SELECT)) {
    launchPath *p = appendTrigger(TRIGGER_TYPE_SELECT);
    if (p)
      p->paths = addStr(p->paths, "hdd0:__sysconf:pfs:/FMCB/BOOT.ELF");
  }
}

// trigger -> CNF 键名 (boot_<key>), 未支持的键返回 NULL
static const char *triggerKey(TriggerType t) {
  switch (t) {
  case TRIGGER_TYPE_AUTO:
    return "auto";
  case TRIGGER_TYPE_START:
    return "start";
  case TRIGGER_TYPE_SELECT:
    return "select";
  case TRIGGER_TYPE_TRIANGLE:
    return "triangle";
  case TRIGGER_TYPE_CIRCLE:
    return "circle";
  case TRIGGER_TYPE_CROSS:
    return "cross";
  case TRIGGER_TYPE_SQUARE:
    return "square";
  case TRIGGER_TYPE_UP:
    return "up";
  case TRIGGER_TYPE_DOWN:
    return "down";
  case TRIGGER_TYPE_LEFT:
    return "left";
  case TRIGGER_TYPE_RIGHT:
    return "right";
  case TRIGGER_TYPE_L1:
    return "l1";
  case TRIGGER_TYPE_L2:
    return "l2";
  case TRIGGER_TYPE_R1:
    return "r1";
  case TRIGGER_TYPE_R2:
    return "r2";
  default:
    return NULL;
  }
}

// 把当前 settings 生成 OSDMBR.CNF 写回配置分区, 成功返回 0
// 注意: fileXioOpen 必须用 FIO_O_* (iomanX) 标志. newlib 头文件的 O_WRONLY=1,
// 而 iomanX 里 1 = 只读, 会导致以只读方式打开, 所有写入静默失败
int saveConfig(void) {
  umountPFS(); // 确保干净的挂载状态

  // 读写模式挂载配置分区
  if (fileXioMount("pfs0:", HOSD_CONF_PARTITION, FIO_MT_RDWR))
    return -1;

  // 确保目录存在 (已存在时返回错误, 忽略)
  fileXioMkdir("pfs0:/osdmenu", 0666);

  int fd = fileXioOpen(configPath, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
  if (fd < 0) {
    fileXioUmount("pfs0:");
    return -2;
  }

  char buf[PATH_MAX];
  int len;
  int ok = 1;

  len = snprintf(buf, sizeof(buf), "timeout = %d\r\n", (int)settings.timeout);
  ok &= (fileXioWrite(fd, buf, len) == len);
  len = snprintf(buf, sizeof(buf), "default_app = %d\r\n", (int)settings.defaultApp);
  ok &= (fileXioWrite(fd, buf, len) == len);
  len = snprintf(buf, sizeof(buf), "cdrom_skip_ps2logo = %d\r\n", (settings.flags & FLAG_SKIP_PS2_LOGO) ? 1 : 0);
  ok &= (fileXioWrite(fd, buf, len) == len);

  for (launchPath *p = settings.paths; p; p = p->next) {
    const char *key = triggerKey(p->trigger);
    if (!key)
      continue;
    for (linkedStr *s = p->paths; s; s = s->next) {
      len = snprintf(buf, sizeof(buf), "boot_%s = %s\r\n", key, s->str);
      ok &= (fileXioWrite(fd, buf, len) == len);
    }
    for (linkedStr *a = p->args; a; a = a->next) {
      len = snprintf(buf, sizeof(buf), "boot_%s_arg = %s\r\n", key, a->str);
      ok &= (fileXioWrite(fd, buf, len) == len);
    }
  }

  if (fileXioClose(fd) < 0)
    ok = 0;
  if (fileXioUmount("pfs0:") < 0)
    ok = 0;
  return ok ? 0 : -3;
}

int loadConfig() {
  // Init default values
  settings.flags = FLAG_ENABLE_GAMEID;
  settings.paths = NULL;
  settings.isPSX = 0;
  settings.osdLanguage = -1;
  settings.osdScreenType = -1;
  settings.timeout = 5; // 默认倒计时 5 秒
  settings.defaultApp = 0; // 默认启动: 文件管理器

  int fd = open("rom0:PSXVER", O_RDONLY);
  if (fd >= 0) {
    DPRINTF("Running on PSX\n");
    close(fd);
    settings.isPSX = 1;
  }

  // Open the config file
  DPRINTF("Loading config from XFROM\n");
  FILE *file = fopen(xfromConfigPath, "rb");
  if (!file) {
    DPRINTF("Loading config from HDD\n");
    // Mount the config partition
    if (mountPFS(HOSD_CONF_PARTITION)) {
      ensureDefaultPaths(); // 无配置分区, 使用内置默认路径
      return -ENODEV;
    }

    // Open the config file
    file = fopen(configPath, "rb");
    if (!file) {
      umountPFS();
      ensureDefaultPaths(); // 无配置文件, 使用内置默认路径
      return -ENOENT;
    }
  }

  launchPath *currentPath = NULL;
  char lineBuffer[PATH_MAX] = {0};
  char *valuePtr = NULL;
  char *optPtr = NULL;
  while (fgets(lineBuffer, sizeof(lineBuffer), file)) { // fgets returns NULL if EOF or an error occurs
    // Find the start of the value
    valuePtr = strchr(lineBuffer, '=');
    if (!valuePtr)
      continue;
    *valuePtr = '\0';

    // Trim whitespace and terminate the value
    do {
      valuePtr++;
    } while (isspace((int)*valuePtr));
    valuePtr[strcspn(valuePtr, "\r\n")] = '\0';

    if (strlen(valuePtr) == 0)
      continue;

    if (!strncmp(lineBuffer, "boot_", 5)) {
      // Handle the path option

      // Get the pointer to _<key>
      optPtr = strchr(lineBuffer, '_');
      if (!optPtr)
        continue;
      optPtr++;

      TriggerType btn;
      if (!strncmp(optPtr, "auto", 4))
        btn = TRIGGER_TYPE_AUTO;
      else if (!strncmp(optPtr, "start", 5))
        btn = TRIGGER_TYPE_START;
      else if (!strncmp(optPtr, "select", 6))
        btn = TRIGGER_TYPE_SELECT;
      else if (!strncmp(optPtr, "triangle", 8))
        btn = TRIGGER_TYPE_TRIANGLE;
      else if (!strncmp(optPtr, "circle", 6))
        btn = TRIGGER_TYPE_CIRCLE;
      else if (!strncmp(optPtr, "cross", 5))
        btn = TRIGGER_TYPE_CROSS;
      else if (!strncmp(optPtr, "square", 6))
        btn = TRIGGER_TYPE_SQUARE;
      else if (!strncmp(optPtr, "up", 2))
        btn = TRIGGER_TYPE_UP;
      else if (!strncmp(optPtr, "down", 4))
        btn = TRIGGER_TYPE_DOWN;
      else if (!strncmp(optPtr, "left", 4))
        btn = TRIGGER_TYPE_LEFT;
      else if (!strncmp(optPtr, "right", 5))
        btn = TRIGGER_TYPE_RIGHT;
      else if (!strncmp(optPtr, "l1", 2))
        btn = TRIGGER_TYPE_L1;
      else if (!strncmp(optPtr, "l2", 2))
        btn = TRIGGER_TYPE_L2;
      else if (!strncmp(optPtr, "r1", 2))
        btn = TRIGGER_TYPE_R1;
      else if (!strncmp(optPtr, "r2", 2))
        btn = TRIGGER_TYPE_R2;
      else
        continue;

      // Search for the existing option associated with the button
      currentPath = settings.paths;
      if (currentPath)
        while (currentPath->next) {
          if (currentPath->trigger == btn)
            break;

          currentPath = currentPath->next;
        }

      if (!currentPath || currentPath->trigger != btn) {
        // If it doesn't exist, initialize the path
        launchPath *newPath = malloc(sizeof(launchPath));
        if (!newPath)
          bootFail("Failed to allocate memory for a path");

        newPath->trigger = btn;
        newPath->paths = NULL;
        newPath->args = NULL;
        newPath->argCount = 0;
        newPath->next = NULL;

        // Insert the path into the list
        if (currentPath)
          currentPath->next = newPath;

        currentPath = newPath;
      }
      if (!settings.paths)
        settings.paths = currentPath;

      // Check if the option has an _arg suffix
      if ((optPtr = strrchr(lineBuffer, '_')) && !strncmp(++optPtr, "arg", 3)) {
        // Add the argument
        currentPath->args = addStr(currentPath->args, valuePtr);
        currentPath->argCount++;
      } else
        // Add the path
        currentPath->paths = addStr(currentPath->paths, valuePtr);

      continue;
    }

    // Handle flags
    if (!strncmp(lineBuffer, "timeout", 7)) {
      // 菜单倒计时秒数
      settings.timeout = (int16_t)atoi(valuePtr);
      continue;
    }
    if (!strncmp(lineBuffer, "default_app", 11)) {
      // 默认启动应用: 0=文件管理器 1=峰哥OPL 2=FMCB菜单
      int d = atoi(valuePtr);
      settings.defaultApp = (d >= 0 && d <= 2) ? (int8_t)d : 0;
      continue;
    }
    if (!strncmp(lineBuffer, "cdrom_skip_ps2logo", 18)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_SKIP_PS2_LOGO;
      else
        settings.flags &= ~(FLAG_SKIP_PS2_LOGO);
      continue;
    }
    if (!strncmp(lineBuffer, "cdrom_disable_gameid", 20)) {
      if (atoi(valuePtr))
        settings.flags &= ~(FLAG_ENABLE_GAMEID);
      else
        settings.flags |= FLAG_ENABLE_GAMEID;
      continue;
    }
    if (!strncmp(lineBuffer, "cdrom_use_dkwdrv", 16)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_USE_DKWDRV;
      else
        settings.flags &= ~(FLAG_USE_DKWDRV);
      continue;
    }
    if (!strncmp(lineBuffer, "ps1drv_enable_fast", 18)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_PS1DRV_FAST;
      else
        settings.flags &= ~(FLAG_PS1DRV_FAST);
      continue;
    }
    if (!strncmp(lineBuffer, "ps1drv_enable_smooth", 20)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_PS1DRV_SMOOTH;
      else
        settings.flags &= ~(FLAG_PS1DRV_SMOOTH);
      continue;
    }
    if (!strncmp(lineBuffer, "ps1drv_use_ps1vn", 16)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_PS1DRV_USE_VN;
      else
        settings.flags &= ~(FLAG_PS1DRV_USE_VN);
      continue;
    }
    if (!strncmp(lineBuffer, "prefer_bbn", 10)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_PREFER_BBN;
      else
        settings.flags &= ~(FLAG_PREFER_BBN);
      continue;
    }
    if (!strncmp(lineBuffer, "app_gameid", 10)) {
      if (atoi(valuePtr))
        settings.flags |= FLAG_APP_GAMEID;
      else
        settings.flags &= ~(FLAG_APP_GAMEID);
      continue;
    }
    if (!strncmp(lineBuffer, "osd_screentype", 14)) {
      if (!strcmp(valuePtr, "4:3"))
        settings.osdScreenType = TV_SCREEN_43;
      else if (!strcmp(valuePtr, "16:9"))
        settings.osdScreenType = TV_SCREEN_169;
      else if (!strcmp(valuePtr, "full"))
        settings.osdScreenType = TV_SCREEN_FULL;
      continue;
    }
    if (!strncmp(lineBuffer, "osd_language", 12)) {
      if (!strcmp(valuePtr, "jap"))
        settings.osdLanguage = LANGUAGE_JAPANESE;
      else if (!strcmp(valuePtr, "eng"))
        settings.osdLanguage = LANGUAGE_ENGLISH;
      else if (!strcmp(valuePtr, "fre"))
        settings.osdLanguage = LANGUAGE_FRENCH;
      else if (!strcmp(valuePtr, "spa"))
        settings.osdLanguage = LANGUAGE_SPANISH;
      else if (!strcmp(valuePtr, "ger"))
        settings.osdLanguage = LANGUAGE_GERMAN;
      else if (!strcmp(valuePtr, "ita"))
        settings.osdLanguage = LANGUAGE_ITALIAN;
      else if (!strcmp(valuePtr, "dut"))
        settings.osdLanguage = LANGUAGE_DUTCH;
      else if (!strcmp(valuePtr, "por"))
        settings.osdLanguage = LANGUAGE_PORTUGUESE;
      else if (!strcmp(valuePtr, "rus"))
        settings.osdLanguage = LANGUAGE_RUSSIAN;
      else if (!strcmp(valuePtr, "kor"))
        settings.osdLanguage = LANGUAGE_KOREAN;
      else if (!strcmp(valuePtr, "tch"))
        settings.osdLanguage = LANGUAGE_TRAD_CHINESE;
      else if (!strcmp(valuePtr, "sch"))
        settings.osdLanguage = LANGUAGE_SIMPL_CHINESE;
      continue;
    }
  }
  fclose(file);
  umountPFS();
  ensureDefaultPaths(); // 用户配置里缺的项用内置默认路径补全
  return 0;
}

// Attempts to load per-title GSM argument from the GSM_CONF_PATH/HOSDGSM_CONF_PATH depending on the device hint
char *getOSDGSMArgument(const char *titleID) {
  if (!titleID)
    return NULL;

  DPRINTF("Loading eGSM config file from XFROM\n");
  FILE *gsmConf = fopen("xfrom:" HOSDGSM_CONF_PATH, "r");
  if (!gsmConf) {
    DPRINTF("Loading eGSM config file from HDD\n");
    if (mountPFS(HOSD_CONF_PARTITION))
      return NULL;

    gsmConf = fopen("pfs0:" HOSDGSM_CONF_PATH, "r");
    if (!gsmConf) {
      umountPFS();
      return NULL;
    }
  }

  char *defaultArg = NULL;
  char *titleArg = NULL;
  char lineBuffer[30] = {0};
  char *valuePtr = NULL;
  while (fgets(lineBuffer, sizeof(lineBuffer), gsmConf)) { // fgets returns NULL if EOF or an error occurs
    // Find the start of the value
    valuePtr = strchr(lineBuffer, '=');
    if (!valuePtr)
      continue;
    *valuePtr = '\0';

    // Trim whitespace and terminate the value
    do {
      valuePtr++;
    } while (isspace((int)*valuePtr));
    valuePtr[strcspn(valuePtr, "\r\n")] = '\0';

    if (!strncmp(lineBuffer, titleID, 11)) {
      DPRINTF("eGSM will use the title-specific config\n");
      titleArg = strdup(valuePtr);
      break;
    }

    if (!strncmp(lineBuffer, "default", 7))
      defaultArg = strdup(valuePtr);
  }

  fclose(gsmConf);
  umountPFS();

  if (titleArg) {
    // If there's a title-specific argument free the defaultArg and set the defaultArg to titleArg
    if (defaultArg)
      free(defaultArg);

    defaultArg = titleArg;
  }
  return defaultArg;
}
