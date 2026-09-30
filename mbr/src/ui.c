// ui.c - anyi 中文引导菜单 + 设置界面
// 主菜单: 启动文件管理器 / 启动峰哥OPL / 启动FMCB菜单 / 返回记忆卡 / 读取DVD / 设置
// 纯蓝渐变背景 + 白色文字, 确认仅限圆圈键
#include "config.h"
#include "disc.h"
#include "dprintf.h"
#include "font_cn.h"
#include "hdd.h"
#include "ui.h"
#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>
#include <libcdvd.h>
#include <libpad.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <fileio.h>
#include <iox_stat.h>

// ---------------- 纹理图集 ----------------
// 1024x64, 8bit 索引纹理 (GS_PSM_T8)
//   行0 (y 0-15)   : 汉字/符号 0-63, x = idx*16
//   行1 (y 16-31)  : ASCII 0x20-0x5F, x = (c-0x20)*8
//   行2 (y 32-47)  : ASCII 0x60-0x7E, x = (c-0x60)*8
//   行3 (y 48-63)  : 汉字/符号 64-73 (第 65 个汉字起换行)
#define ATLAS_W 1024
#define ATLAS_H 64

static unsigned char atlas[ATLAS_W * ATLAS_H] __attribute__((aligned(64)));
static unsigned int atlas_clut[256] __attribute__((aligned(64)));

static GSGLOBAL *gsGlobal = NULL;
static GSTEXTURE fontTex;

// 运行时把点阵字库展开为纹理图集
static void buildAtlas(void) {
  memset(atlas, 0, sizeof(atlas));

  // 汉字: 每行2字节, MSB=最左像素; 超过64个后放到行3
  for (int i = 0; i < FONT_CN_COUNT; i++) {
    int gx = (i & 63) * 16;
    int gy = (i < 64) ? 0 : 48;
    for (int y = 0; y < 16; y++) {
      unsigned int row = ((unsigned int)font_cn[i][y * 2] << 8) | font_cn[i][y * 2 + 1];
      unsigned char *dst = &atlas[(gy + y) * ATLAS_W + gx];
      for (int x = 0; x < 16; x++)
        dst[x] = (row & (0x8000u >> x)) ? 1 : 0;
    }
  }

  // ASCII: 每行1字节, MSB=最左像素
  for (int c = 0; c < 95; c++) {
    int ax = (c < 64) ? c * 8 : (c - 64) * 8;
    int ay = (c < 64) ? 16 : 32;
    for (int y = 0; y < 16; y++) {
      unsigned int row = font_ascii[c][y];
      unsigned char *dst = &atlas[(ay + y) * ATLAS_W + ax];
      for (int x = 0; x < 8; x++)
        dst[x] = (row & (0x80u >> x)) ? 1 : 0;
    }
  }

  // CLUT: 索引0透明, 索引1白色不透明
  memset(atlas_clut, 0, sizeof(atlas_clut));
  atlas_clut[1] = 0x80FFFFFF;
}

// ---------------- 文本绘制 (UTF-8 中文 + ASCII 混排) ----------------
// 支持码点查找: 2字节(×等)与3字节(汉字)UTF-8均查 font_cn 表
static int glyphIndex(unsigned int cp) {
  for (int i = 0; i < FONT_CN_COUNT; i++) {
    if (font_cn_codepoints[i] == cp)
      return i;
  }
  return -1;
}

// 汉字索引 -> 图集 UV (0-63 在行0, 64+ 在行3)
static void glyphUV(int gi, float *u0, float *v0) {
  if (gi < 64) {
    *u0 = (float)(gi * 16);
    *v0 = 0.0f;
  } else {
    *u0 = (float)((gi - 64) * 16);
    *v0 = 48.0f;
  }
}

// 返回文本像素宽度 (scale=1: 汉字16/ASCII 8)
static float textWidth(const char *utf8, int scale) {
  const unsigned char *p = (const unsigned char *)utf8;
  float w = 0;
  while (*p) {
    if (*p < 0x80) {
      if (*p >= 0x20)
        w += 8 * scale;
      p += 1;
    } else if ((*p & 0xF0) == 0xE0) {
      w += 16 * scale;
      p += 3;
    } else if ((*p & 0xE0) == 0xC0) {
      unsigned int cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      if (glyphIndex(cp) >= 0)
        w += 16 * scale;
      p += 2;
    } else {
      p += 1;
    }
  }
  return w;
}

// 绘制一行文本, 返回结束 x 坐标
static float drawText(float x, float y, const char *utf8, int scale, u64 color) {
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    float w, h;
    if (*p < 0x80) {
      if (*p < 0x20) { // 控制字符跳过
        p += 1;
        continue;
      }
      int idx = *p - 0x20;
      float u0 = (float)((idx < 64) ? idx * 8 : (idx - 64) * 8);
      float v0 = (float)((idx < 64) ? 16 : 32);
      w = 8;
      h = 16;
      gsKit_prim_sprite_texture(gsGlobal, &fontTex, x, y, u0, v0, x + w * scale, y + h * scale, u0 + w, v0 + h, 0,
                                color);
      x += 8 * scale;
      p += 1;
    } else if ((*p & 0xF0) == 0xE0) {
      // 3字节 UTF-8 -> 码点 -> 查字库
      unsigned int cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      int gi = glyphIndex(cp);
      if (gi >= 0) {
        float u0, v0;
        glyphUV(gi, &u0, &v0);
        w = 16;
        h = 16;
        gsKit_prim_sprite_texture(gsGlobal, &fontTex, x, y, u0, v0, x + w * scale, y + h * scale, u0 + w, v0 + h, 0,
                                  color);
        x += 16 * scale;
      }
      p += 3;
    } else if ((*p & 0xE0) == 0xC0) {
      // 2字节 UTF-8 (× 等)
      unsigned int cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      int gi = glyphIndex(cp);
      if (gi >= 0) {
        float u0, v0;
        glyphUV(gi, &u0, &v0);
        w = 16;
        h = 16;
        gsKit_prim_sprite_texture(gsGlobal, &fontTex, x, y, u0, v0, x + w * scale, y + h * scale, u0 + w, v0 + h, 0,
                                  color);
        x += 16 * scale;
      }
      p += 2;
    } else {
      p += 1;
    }
  }
  return x;
}

// 居中绘制
static void drawTextCentered(float cx, float y, const char *utf8, int scale, u64 color) {
  drawText(cx - textWidth(utf8, scale) / 2, y, utf8, scale, color);
}

// 绘制 ASCII 字符串, 超过 maxChars 字符截断显示 "...", 非 ASCII 显示 '?'
static void drawAsciiTrunc(float x, float y, const char *s, int maxChars, u64 color) {
  char buf[80];
  int n = 0;
  while (s[n] && n < maxChars && n < (int)sizeof(buf) - 2) {
    unsigned char c = (unsigned char)s[n];
    buf[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    n++;
  }
  if (s[n]) { // 被截断
    if (n > 3) {
      buf[n - 3] = '.';
      buf[n - 2] = '.';
      buf[n - 1] = '.';
    } else {
      buf[n++] = '.';
    }
  }
  buf[n] = '\0';
  drawText(x, y, buf, 1, color);
}

// ---------------- 手柄 ----------------
static char pad1Buf[256] __attribute__((aligned(64)));
static char pad2Buf[256] __attribute__((aligned(64)));
static int padReady = 0;

static void padOpen(void) {
  padReady = 0;
  if (padInit(0) != 1)
    return;
  int retries = 10;
  while (retries--) {
    if (padPortOpen(0, 0, pad1Buf) | padPortOpen(1, 0, pad2Buf)) {
      padReady = 1;
      return;
    }
  }
}

static void padClose(void) {
  if (!padReady)
    return;
  padPortClose(0, 0);
  padPortClose(1, 0);
  padEnd();
  padReady = 0;
}

// 返回当前按下的按键位图 (按下=1), 无手柄返回 0
static uint32_t padPoll(void) {
  struct padButtonStatus buttons;
  uint32_t result = 0;
  if (!padReady)
    return 0;
  for (int i = 0; i < 2; i++) {
    if (padRead(i, 0, &buttons) != 0)
      result |= (0xffff ^ buttons.btns);
  }
  return result;
}

// 按下沿 + 长按重复 (方向键按住 25 帧后每 5 帧重复一次)
static uint32_t padRepeat(uint32_t cur, uint32_t prev, int *hold) {
  uint32_t pressed = cur & ~prev;
  uint32_t dirs = cur & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
  if (dirs) {
    if (*hold < 0x7FFF)
      (*hold)++;
    if (*hold > 25 && (*hold % 5) == 0)
      pressed |= dirs;
  } else
    *hold = 0;
  return pressed;
}

// ---------------- 布局与配色 ----------------
#define SCREEN_W 640.0f

// 纯蓝垂直渐变 (NTSC/PAL 全屏覆盖): 顶部亮蓝 -> 底部深蓝
static void drawBackground(void) {
  float h = (float)gsGlobal->Height;
  int rows = 32;
  for (int i = 0; i < rows; i++) {
    float y0 = h * (float)i / rows;
    float y1 = h * (float)(i + 1) / rows;
    int t = i * 255 / (rows - 1);
    int r = 28 - (20 * t >> 8);
    int g = 96 - (72 * t >> 8);
    int b = 224 - (120 * t >> 8);
    gsKit_prim_sprite(gsGlobal, 0, y0, SCREEN_W, y1, 0, GS_SETREG_RGBA(r, g, b, 0x80));
  }
}

// 白色系配色 (错误用淡红区分)
#define COL_TITLE GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x80)
#define COL_ITEM GS_SETREG_RGBA(0xD0, 0xDC, 0xF0, 0x80)
#define COL_ITEM_SEL GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x80)
#define COL_VALUE GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x80)
#define COL_HINT GS_SETREG_RGBA(0xC4, 0xD0, 0xE8, 0x80)
#define COL_DIR GS_SETREG_RGBA(0xE8, 0xF4, 0xFF, 0x80)
#define COL_ERR GS_SETREG_RGBA(0xFF, 0xA0, 0xA0, 0x80)
#define COL_OK GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x80)

// 选中项高亮: 半透明白
#define SEL_BAR GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x38)

// 简易数字转字符串 (0-99)
static int numToStr(int v, char *buf) {
  int n = 0;
  if (v < 0)
    v = 0;
  if (v > 99)
    v = 99;
  if (v >= 10)
    buf[n++] = '0' + (v / 10) % 10;
  buf[n++] = '0' + v % 10;
  buf[n] = '\0';
  return n;
}

// ---------------- settings 路径读写 ----------------
static launchPath *uiFindTrigger(TriggerType t) {
  launchPath *p = settings.paths;
  while (p) {
    if (p->trigger == t)
      return p;
    p = p->next;
  }
  return NULL;
}

static const char *uiGetFirstPath(TriggerType t) {
  launchPath *p = uiFindTrigger(t);
  if (p && p->paths)
    return p->paths->str;
  return "";
}

// 替换指定按键的第一条启动路径, 保留其余备用路径
static void uiSetFirstPath(TriggerType t, const char *path) {
  launchPath *p = uiFindTrigger(t);
  if (!p)
    return;
  if (p->paths) {
    linkedStr *nu = malloc(sizeof(linkedStr));
    if (!nu)
      return;
    nu->str = strdup(path);
    nu->next = p->paths->next;
    linkedStr *old = p->paths;
    p->paths = nu;
    free(old->str);
    free(old);
  } else
    p->paths = addStr(NULL, (char *)path);
}

// ---------------- 默认启动应用 ----------------
// settings.defaultApp: 0=文件管理器 1=峰哥OPL 2=FMCB菜单
static const char *const defaultAppNames[3] = {"文件管理器", "OPL游戏菜单", "FMCB菜单"};

// 默认启动应用索引 (0-2), 越界回 0
static int defaultAppIdx(void) {
  return (settings.defaultApp >= 0 && settings.defaultApp <= 2) ? settings.defaultApp : 0;
}

// 默认启动应用 -> 对应启动按键
static TriggerType defaultAppTrigger(void) {
  switch (defaultAppIdx()) {
  case 1:
    return TRIGGER_TYPE_START;
  case 2:
    return TRIGGER_TYPE_SELECT;
  default:
    return TRIGGER_TYPE_AUTO;
  }
}

// ---------------- 硬盘文件浏览 ----------------
typedef struct {
  char name[64];
  int isDir;
} DirEntry;

static DirEntry browseEntries[128];

static int cmpEntry(const void *a, const void *b) {
  const DirEntry *ea = a, *eb = b;
  if (ea->isDir != eb->isDir)
    return ea->isDir ? -1 : 1; // 目录在前
  return strcmp(ea->name, eb->name);
}

static int isElfName(const char *name) {
  int n = strlen(name);
  if (n < 4)
    return 0;
  const char *ext = name + n - 4;
  return ext[0] == '.' && (ext[1] == 'E' || ext[1] == 'e') && (ext[2] == 'L' || ext[2] == 'l') &&
         (ext[3] == 'F' || ext[3] == 'f');
}

// 安全拷贝名字 (显式截断, 避免编译器截断警告)
static void copyName(char *dst, size_t dstSize, const char *src) {
  size_t len = strlen(src);
  if (len >= dstSize)
    len = dstSize - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

// 列出硬盘全部分区, 返回数量 (0 = 失败/无)
static int listPartitions(DirEntry *out, int max) {
  int dfd = fileXioDopen("hdd0:");
  if (dfd < 0)
    return 0;
  iox_dirent_t de;
  int n = 0;
  while (fileXioDread(dfd, &de) > 0) {
    if (de.name[0] == '\0')
      continue;
    if (n >= max)
      break;
    copyName(out[n].name, sizeof(out[n].name), de.name);
    out[n].isDir = 1; // 分区可进入
    n++;
  }
  fileXioDclose(dfd);
  qsort(out, n, sizeof(DirEntry), cmpEntry);
  return n;
}

// 列出已挂载 pfs0: 下指定目录的子目录与 .ELF 文件, 返回数量 (-1 = 打开失败)
static int listDir(const char *pfsPath, DirEntry *out, int max) {
  int dfd = fileXioDopen(pfsPath);
  if (dfd < 0)
    return -1;
  iox_dirent_t de;
  int n = 0;
  while (fileXioDread(dfd, &de) > 0) {
    if (de.name[0] == '\0' || !strcmp(de.name, ".") || !strcmp(de.name, ".."))
      continue;
    int isDir = (de.stat.mode & FIO_S_IFDIR) != 0;
    if (!isDir && !isElfName(de.name))
      continue; // 只显示目录和 ELF
    if (n >= max)
      break;
    copyName(out[n].name, sizeof(out[n].name), de.name);
    out[n].isDir = isDir;
    n++;
  }
  fileXioDclose(dfd);
  qsort(out, n, sizeof(DirEntry), cmpEntry);
  return n;
}

// 文件浏览界面: 选分区 -> 选目录/ELF
// 返回 malloc 的完整路径 "hdd0:<分区>:pfs:<绝对路径>", 取消返回 NULL
static char *uiBrowseLoop(void) {
  int modeParts = 1; // 1=分区列表, 0=目录浏览
  int count = listPartitions(browseEntries, 128);
  int sel = 0, offset = 0;
  int msgTimer = 0; // >0 显示 "无法打开"
  char part[64] = "";
  char cwd[192] = "";
  char pfsPath[304];
  uint32_t prev = 0;
  int hold = 0;

  while (1) {
    uint32_t cur = padPoll();
    uint32_t pressed = padRepeat(cur, prev, &hold);
    prev = cur;

    if (pressed & PAD_UP)
      sel--;
    if (pressed & PAD_DOWN)
      sel++;
    if (sel < 0)
      sel = count > 0 ? count - 1 : 0;
    if (sel >= count)
      sel = 0;
    if (count > 11) {
      offset = sel - 5;
      if (offset < 0)
        offset = 0;
      if (offset > count - 11)
        offset = count - 11;
    } else
      offset = 0;

    if ((pressed & PAD_CIRCLE) && count > 0) {
      DirEntry *e = &browseEntries[sel];
      if (modeParts) {
        // 挂载选中的分区并进入
        char dev[80];
        snprintf(dev, sizeof(dev), "hdd0:%s", e->name);
        umountPFS();
        if (fileXioMount("pfs0:", dev, FIO_MT_RDONLY) == 0) {
          int n = listDir("pfs0:/", browseEntries, 128);
          if (n >= 0) {
            copyName(part, sizeof(part), e->name);
            strcpy(cwd, "/");
            count = n;
            modeParts = 0;
            sel = 0;
          } else {
            umountPFS();
            msgTimer = 90; // 分区打开失败, 保持分区列表
          }
        } else
          msgTimer = 90; // 挂载失败
      } else if (e->isDir) {
        // 进入子目录 (成功后才提交 cwd)
        char newcwd[288];
        snprintf(newcwd, sizeof(newcwd), "%s%s%s", cwd, !strcmp(cwd, "/") ? "" : "/", e->name);
        snprintf(pfsPath, sizeof(pfsPath), "pfs0:%s", newcwd);
        int n = listDir(pfsPath, browseEntries, 128);
        if (n < 0)
          msgTimer = 90;
        else {
          strcpy(cwd, newcwd);
          count = n;
          sel = 0;
        }
      } else {
        // 选中 ELF 文件, 返回完整路径
        char full[448];
        snprintf(full, sizeof(full), "hdd0:%s:pfs:%s%s%s", part, cwd, !strcmp(cwd, "/") ? "" : "/", e->name);
        umountPFS();
        return strdup(full);
      }
    }

    if (pressed & PAD_CROSS) {
      if (modeParts)
        return NULL; // 取消
      if (!strcmp(cwd, "/")) {
        // 已在分区根目录, 返回分区列表
        umountPFS();
        modeParts = 1;
        count = listPartitions(browseEntries, 128);
        sel = 0;
      } else {
        // 返回上级目录 (成功后才提交 cwd)
        char newcwd[288];
        strcpy(newcwd, cwd);
        char *lastSlash = strrchr(newcwd, '/');
        if (lastSlash == newcwd)
          strcpy(newcwd, "/");
        else
          *lastSlash = '\0';
        snprintf(pfsPath, sizeof(pfsPath), "pfs0:%s", newcwd);
        int n = listDir(pfsPath, browseEntries, 128);
        if (n < 0)
          msgTimer = 90;
        else {
          strcpy(cwd, newcwd);
          count = n;
          sel = 0;
        }
      }
    }

    // ---- 绘制一帧 ----
    drawBackground();
    drawTextCentered(320, 30, "选择启动文件", 2, COL_TITLE);

    if (modeParts) {
      drawText(40, 80, "分区:", 1, COL_HINT);
      if (count == 0 && msgTimer <= 0)
        drawText(200, 80, "(无)", 1, COL_HINT);
    } else {
      char pos[300];
      snprintf(pos, sizeof(pos), "hdd0:%s:pfs:%s", part, cwd);
      drawAsciiTrunc(40, 80, pos, 70, COL_HINT);
    }

    int rows = count > 11 ? 11 : count;
    for (int i = 0; i < rows; i++) {
      int idx = offset + i;
      float y = 110 + i * 26;
      if (idx == sel)
        gsKit_prim_sprite(gsGlobal, 40, y - 5, 600, y + 21, 0, SEL_BAR);
      u64 col = browseEntries[idx].isDir ? COL_DIR : COL_ITEM_SEL;
      drawAsciiTrunc(56, y, browseEntries[idx].name, 48, col);
      if (browseEntries[idx].isDir && !modeParts)
        drawText(552, y, "<目录>", 1, COL_DIR);
    }

    if (count == 0 && msgTimer > 0)
      drawTextCentered(320, 240, "无法打开", 2, COL_ERR);
    else if (count == 0 && !modeParts)
      drawTextCentered(320, 240, "空目录", 2, COL_HINT);

    drawTextCentered(320, 424, "○ 进入 × 返回", 1, COL_HINT);

    if (msgTimer > 0)
      msgTimer--;

    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);
  }
}

// ---------------- 设置界面 ----------------
// 倒计时值文本
static void timeoutValueText(char *buf, int size) {
  if (settings.timeout < 0)
    strcpy(buf, "等待选择");
  else if (settings.timeout == 0)
    strcpy(buf, "直接进入");
  else {
    numToStr(settings.timeout, buf);
    strcat(buf, " 秒后自动进入");
  }
}

// 设置行 -> 对应启动按键
static TriggerType settingTrigger(int row) {
  switch (row) {
  case 0:
    return TRIGGER_TYPE_AUTO;
  case 1:
    return TRIGGER_TYPE_START;
  case 2:
    return TRIGGER_TYPE_SELECT;
  default:
    return TRIGGER_TYPE_AUTO;
  }
}

static void uiSettingsLoop(void) {
  int sel = 0;
  uint32_t prev = 0;
  int hold = 0;
  int msgTimer = 0; // 保存结果提示剩余帧数
  int msgOk = 0;

  while (1) {
    uint32_t cur = padPoll();
    uint32_t pressed = padRepeat(cur, prev, &hold);
    prev = cur;

    if (msgTimer > 0) {
      msgTimer--;
      if (msgTimer == 0 && msgOk)
        return; // 保存成功提示结束, 退出设置
    } else {
      if (pressed & PAD_UP)
        sel = (sel + 6) % 7;
      if (pressed & PAD_DOWN)
        sel = (sel + 1) % 7;

      if (sel == 3 && (pressed & (PAD_LEFT | PAD_RIGHT))) { // 默认启动, 循环切换
        int d = settings.defaultApp + ((pressed & PAD_RIGHT) ? 1 : -1);
        if (d < 0)
          d = 2;
        if (d > 2)
          d = 0;
        settings.defaultApp = (int8_t)d;
      }

      if (sel == 4 && (pressed & (PAD_LEFT | PAD_RIGHT))) { // 倒计时
        int t = settings.timeout + ((pressed & PAD_RIGHT) ? 1 : -1);
        if (t < -1)
          t = -1;
        if (t > 99)
          t = 99;
        settings.timeout = (int16_t)t;
      }

      if (pressed & PAD_CROSS)
        return; // 返回不保存

      if (pressed & PAD_CIRCLE) {
        switch (sel) {
        case 0:
        case 1:
        case 2: {
          char *picked = uiBrowseLoop();
          if (picked) {
            uiSetFirstPath(settingTrigger(sel), picked);
            free(picked);
          }
          prev = padPoll(); // 刷新按键状态, 防止浏览退出按键泄漏
          hold = 0;
          break;
        }
        case 5: // 保存并返回
          msgOk = (saveConfig() == 0);
          msgTimer = msgOk ? 100 : 150;
          break;
        case 6: // 返回不保存
          return;
        }
      }
    }

    // ---- 绘制一帧 ----
    drawBackground();
    drawTextCentered(320, 40, "设置", 2, COL_TITLE);
    gsKit_prim_sprite(gsGlobal, 260, 86, 380, 87, 0, GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x50));

    const char *labels[7] = {"文件管理器路径", "OPL 路径",     "FMCB 菜单路径", "默认启动",
                             "倒计时",         "保存并返回",   "返回不保存"};
    float rowY[7] = {108, 148, 188, 228, 268, 308, 348};
    for (int i = 0; i < 7; i++) {
      if (i == sel && msgTimer == 0)
        gsKit_prim_sprite(gsGlobal, 64, rowY[i] - 8, 600, rowY[i] + 24, 0, SEL_BAR);
      u64 col = (i == sel) ? COL_ITEM_SEL : COL_ITEM;
      drawText(80, rowY[i], labels[i], 1, col);
      if (i < 3)
        drawAsciiTrunc(300, rowY[i], uiGetFirstPath(settingTrigger(i)), 37, COL_VALUE);
      else if (i == 3)
        drawText(300, rowY[i], defaultAppNames[defaultAppIdx()], 1, COL_VALUE);
      else if (i == 4) {
        char buf[48];
        timeoutValueText(buf, sizeof(buf));
        drawText(300, rowY[i], buf, 1, COL_VALUE);
      }
    }

    if (msgTimer > 0)
      drawTextCentered(320, 390, msgOk ? "保存成功" : "保存失败", 1, msgOk ? COL_OK : COL_ERR);
    else
      drawTextCentered(320, 424, "上下选择 ○ 确认 × 返回 左右调节", 1, COL_HINT);

    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);
  }
}

// ---------------- 主菜单 ----------------
// 预检光驱: 0=PS1/PS2游戏盘, -1=无光盘, -2=非游戏光盘
static int checkGameDisc(void) {
  if (!sceCdInit(SCECdINIT))
    return -2;
  sceCdDiskReady(0);
  int type = sceCdGetDiskType();
  sceCdInit(SCECdEXIT);
  if (type >= SCECdNODISC && type < SCECdUNKNOWN)
    return -1;
  if (type >= SCECdPSCD && type <= SCECdPS2DVD)
    return 0;
  return -2;
}

// 关闭手柄和图形, 为跳转其他程序做准备
static void uiTeardown(void) {
  padClose();
  gsKit_deinit_global(gsGlobal);
  gsGlobal = NULL;
}

// 菜单循环: 返回选中的 TriggerType
TriggerType uiMenuLoop(void) {
  // timeout = 0: 不显示菜单, 直接进默认启动应用
  if (settings.timeout == 0)
    return defaultAppTrigger();

  buildAtlas();

  // GS + DMA 初始化
  gsGlobal = gsKit_init_global();
  gsGlobal->DoubleBuffering = GS_SETTING_ON;
  dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
              1 << DMA_CHANNEL_GIF);
  if (dmaKit_chan_init(DMA_CHANNEL_GIF))
    return TRIGGER_TYPE_AUTO;

  gsKit_init_screen(gsGlobal);
  gsKit_display_buffer(gsGlobal);
  gsKit_mode_switch(gsGlobal, GS_ONESHOT);

  // 启用 alpha 混合 (wLaunchELF/RetroArch 同款三行):
  // 字库透明像素与半透明高亮条透出蓝色背景, 消除文字黑色底块
  gsKit_set_primalpha(gsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
  gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
  gsKit_set_test(gsGlobal, GS_ATEST_ON);

  // 上传字库纹理 (仅一次)
  // 必须先用 gsKit_vram_alloc 分配显存: Vram/VramClut=0 会指向帧缓冲,
  // 导致文字渲染成反馈条纹
  memset(&fontTex, 0, sizeof(fontTex));
  fontTex.Width = ATLAS_W;
  fontTex.Height = ATLAS_H;
  fontTex.PSM = GS_PSM_T8;
  fontTex.ClutPSM = GS_PSM_CT32; // T8 需要 256 项 32 位调色板
  fontTex.Mem = (u32 *)atlas;
  fontTex.Clut = (u32 *)atlas_clut;
  fontTex.Filter = GS_FILTER_NEAREST;
  fontTex.Vram = gsKit_vram_alloc(gsGlobal, gsKit_texture_size(ATLAS_W, ATLAS_H, GS_PSM_T8), GSKIT_ALLOC_USERBUFFER);
  fontTex.VramClut = gsKit_vram_alloc(gsGlobal, 256 * 4, GSKIT_ALLOC_USERBUFFER);
  if (fontTex.Vram == GSKIT_ALLOC_ERROR || fontTex.VramClut == GSKIT_ALLOC_ERROR)
    return TRIGGER_TYPE_AUTO;
  gsKit_texture_upload(gsGlobal, &fontTex);

  padOpen();

  int fps = (gsGlobal->Mode == GS_MODE_PAL) ? 50 : 60;
  long framesLeft = (long)settings.timeout * fps;
  int sel = 0;
  int countdown = settings.timeout > 0; // >0 才倒计时; <=0 无限等待, 由用户选择
  int errTimer = 0;                     // >0 显示错误提示 (暂停倒计时)
  const char *errMsg = NULL;
  uint32_t prev = 0;
  int hold = 0;

  while (1) {
    uint32_t cur = padPoll();
    uint32_t pressed = padRepeat(cur, prev, &hold);
    prev = cur;

    if (pressed & PAD_UP)
      sel = (sel + 5) % 6;
    if (pressed & PAD_DOWN)
      sel = (sel + 1) % 6;
    if (pressed & (PAD_UP | PAD_DOWN))
      countdown = 0; // 按键后停止倒计时

    if (pressed & PAD_CIRCLE) { // 确认固定仅限圆圈键
      if (sel == 3) { // 返回记忆卡 (PS2 浏览器)
        uiTeardown();
        shutdownDEV9();
        char *args[] = {"BootBrowser"};
        ExecOSD(1, args); // 成功不返回
        return TRIGGER_TYPE_AUTO;
      }
      if (sel == 4) { // 读取DVD
        int st = checkGameDisc();
        if (st == 0) {
          uiTeardown();
          startGameDisc(); // 成功不返回; 失败显示错误并回浏览器
          return TRIGGER_TYPE_AUTO;
        }
        errMsg = (st == -1) ? "无光盘" : "非游戏光盘";
        errTimer = 120;
        continue;
      }
      if (sel == 5) {
        uiSettingsLoop(); // 设置完成后回到菜单
        prev = padPoll(); // 刷新按键状态, 防止设置退出按键泄漏
        hold = 0;
        framesLeft = (long)settings.timeout * fps;
        countdown = settings.timeout > 0;
        if (settings.timeout == 0) { // 设置为直接进入, 立即生效
          uiTeardown();
          return defaultAppTrigger();
        }
        continue;
      }
      break; // sel 0/1/2 -> 启动对应程序
    }
    if (errTimer > 0)
      errTimer--; // 错误提示期间暂停倒计时
    else if (countdown && --framesLeft <= 0) {
      sel = defaultAppIdx(); // 超时自动进默认启动应用
      break;
    }

    // ---- 绘制一帧 ----
    drawBackground();

    // 顶部小字 + 主标题 + 分隔线
    drawTextCentered(320, 34, "anyi 制作", 1, COL_HINT);
    drawTextCentered(320, 62, "硬盘启动菜单", 2, COL_TITLE);
    gsKit_prim_sprite(gsGlobal, 220, 112, 420, 113, 0, GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x50));

    // 菜单项
    const char *menuText[6] = {"启动文件管理器", "启动OPL游戏菜单", "启动FMCB菜单", "返回记忆卡", "读取DVD", "设置"};
    float rowY[6] = {132, 176, 220, 264, 308, 352};
    for (int i = 0; i < 6; i++) {
      if (i == sel)
        gsKit_prim_sprite(gsGlobal, 160, rowY[i] - 6, 480, rowY[i] + 38, 0, SEL_BAR);
      drawTextCentered(320, rowY[i], menuText[i], 2, (i == sel) ? COL_ITEM_SEL : COL_ITEM);
    }

    // 倒计时 / 错误提示
    if (errTimer > 0)
      drawTextCentered(320, 402, errMsg, 1, COL_ERR);
    else if (countdown) {
      char line[48];
      int n = numToStr((int)((framesLeft + fps - 1) / fps), line);
      strcpy(line + n, " 秒后自动进入");
      strcat(line, defaultAppNames[defaultAppIdx()]);
      drawTextCentered(320, 402, line, 1, COL_VALUE);
    }

    // 底部提示
    drawTextCentered(320, 424, "上下选择 ○ 确认", 1, COL_HINT);

    gsKit_queue_exec(gsGlobal);
    gsKit_sync_flip(gsGlobal);
  }

  uiTeardown();

  return (sel == 1) ? TRIGGER_TYPE_START : (sel == 2 ? TRIGGER_TYPE_SELECT : TRIGGER_TYPE_AUTO);
}
