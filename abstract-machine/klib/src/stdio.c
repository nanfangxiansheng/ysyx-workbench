#include <klib.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

// ==================== printf格式化输出 ====================
// 输出缓冲区抽象: buf=NULL时直接通过putch流式输出(printf用),
// 否则写入缓冲区(snprintf/sprintf用), len始终统计完整长度
struct obuf {
  char *buf;
  size_t cap;
  int len;
};

static void ob_putc(struct obuf *o, char c) {
  if (o->buf != NULL) {
    if ((size_t)o->len + 1 < o->cap) { // 留1字节给'\0'
      o->buf[o->len] = c;
    }
  } else {
    putch(c);
  }
  o->len++;
}

static void ob_puts(struct obuf *o, const char *s) {
  while (*s != '\0') {
    ob_putc(o, *s++);
  }
}

// 输出一个无符号整数: base进制, upper用大写字母, neg输出负号,
// width最小字段宽度, prec最小数字位数(<0表示未指定), flags见下方宏
#define FLG_ZERO 0x1  // '0': 用0填充
#define FLG_MINUS 0x2 // '-': 左对齐
#define FLG_PLUS 0x4  // '+': 正数也输出符号
#define FLG_SPACE 0x8 // ' ': 正数用空格占位

static void ob_num(struct obuf *o, unsigned long long v, int base, int upper,
                   int neg, int width, int prec, int flags) {
  char tmp[24]; // 2^64的八进制最长22位
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int nd = 0;
  do {
    tmp[nd++] = dig[v % base];
    v /= base;
  } while (v != 0);
  while (nd < prec && nd < (int)sizeof(tmp)) {
    tmp[nd++] = '0'; // 精度要求的最小数字位数
  }

  char sign = 0;
  if (neg) {
    sign = '-';
  } else if (flags & FLG_PLUS) {
    sign = '+';
  } else if (flags & FLG_SPACE) {
    sign = ' ';
  }

  int total = nd + (sign != 0 ? 1 : 0);
  if ((flags & FLG_MINUS) == 0) {
    if ((flags & FLG_ZERO) && prec < 0) { // 右对齐+零填充
      if (sign != 0) {
        ob_putc(o, sign);
      }
      for (int i = total; i < width; i++) {
        ob_putc(o, '0');
      }
    } else { // 右对齐+空格填充
      for (int i = total; i < width; i++) {
        ob_putc(o, ' ');
      }
      if (sign != 0) {
        ob_putc(o, sign);
      }
    }
  } else if (sign != 0) {
    ob_putc(o, sign);
  }
  while (nd > 0) {
    ob_putc(o, tmp[--nd]);
  }
  if (flags & FLG_MINUS) { // 左对齐, 右侧补空格
    for (int i = total; i < width; i++) {
      ob_putc(o, ' ');
    }
  }
}

// 输出%f: 用IEEE754位操作+纯整数运算实现, 不依赖浮点指令
// value = m * 2^e, 整数部分直接移出, 小数部分逐位乘10取出
static void ob_double(struct obuf *o, double v, int width, int prec, int flags) {
  if (prec < 0) {
    prec = 6; // 默认6位小数
  }
  if (prec > 9) {
    prec = 9; // 逐位乘10的算法最多支持9位, 防止中间结果溢出
  }

  union {
    double d;
    unsigned long long u;
  } cvt = {.d = v};
  int sign = (int)(cvt.u >> 63);
  int exp = (int)((cvt.u >> 52) & 0x7ff);
  unsigned long long frac = cvt.u & 0x000fffffffffffffULL;

  if (exp == 0x7ff) { // inf或nan
    if (sign != 0) {
      ob_putc(o, '-');
    }
    ob_puts(o, frac == 0 ? "inf" : "nan");
    return;
  }

  unsigned long long m; // 有效数字(含隐含位)
  int e;                // value = m * 2^e
  if (exp == 0) {       // 零或次正规数
    m = frac;
    e = -1074;
  } else {
    m = frac | (1ULL << 52);
    e = exp - 1075;
  }

  unsigned long long ipart; // 整数部分
  unsigned long long rem;   // 小数部分: rem / 2^s
  int s;
  if (e >= 0) {
    if (e > 10) { // 数值 >= 2^63, 超出无符号64位范围 (coremark不会遇到)
      if (sign != 0) {
        ob_putc(o, '-');
      }
      ob_puts(o, "overflow");
      return;
    }
    ipart = m << e;
    rem = 0;
    s = 0;
  } else {
    ipart = m >> (-e);
    rem = m & (((unsigned long long)1 << (-e)) - 1);
    s = -e;
  }

  char tmp[24]; // 整数部分
  int nd = 0;
  do {
    tmp[nd++] = '0' + (int)(ipart % 10);
    ipart /= 10;
  } while (ipart != 0);

  int total = nd + (sign != 0 ? 1 : 0) + 1 + prec;
  if ((flags & FLG_MINUS) == 0 && (flags & FLG_ZERO) == 0) {
    for (int i = total; i < width; i++) {
      ob_putc(o, ' ');
    }
  }
  if (sign != 0) {
    ob_putc(o, '-');
  }
  if ((flags & FLG_MINUS) == 0 && (flags & FLG_ZERO) != 0) {
    for (int i = total; i < width; i++) {
      ob_putc(o, '0');
    }
  }
  while (nd > 0) {
    ob_putc(o, tmp[--nd]);
  }
  ob_putc(o, '.');
  for (int i = 0; i < prec; i++) { // 小数部分逐位乘10取出
    if (s > 60) {                  // 剩余部分太小, 按0处理
      ob_putc(o, '0');
      continue;
    }
    unsigned long long r10 = rem * 10;
    ob_putc(o, '0' + (int)(r10 >> s));
    rem = r10 & (((unsigned long long)1 << s) - 1);
  }
  if (flags & FLG_MINUS) {
    for (int i = total; i < width; i++) {
      ob_putc(o, ' ');
    }
  }
}

// 格式化核心: 解析fmt, 从ap中取参数, 输出到o
// 支持转换: %d %i %u %o %x %X %c %s %p %f %%; 支持长度修饰l/ll/h/hh
static void fmt_core(struct obuf *o, const char *fmt, va_list ap) {
  for (; *fmt != '\0'; fmt++) {
    if (*fmt != '%') {
      ob_putc(o, *fmt);
      continue;
    }
    fmt++;

    // 1. 解析flag
    int flags = 0;
    for (;; fmt++) {
      if (*fmt == '0') {
        flags |= FLG_ZERO;
      } else if (*fmt == '-') {
        flags |= FLG_MINUS;
      } else if (*fmt == '+') {
        flags |= FLG_PLUS;
      } else if (*fmt == ' ') {
        flags |= FLG_SPACE;
      } else if (*fmt == '#') {
        // 未使用, 忽略
      } else {
        break;
      }
    }

    // 2. 解析字段宽度
    int width = 0;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      fmt++;
    } else {
      while (isdigit(*fmt)) {
        width = width * 10 + (*fmt - '0');
        fmt++;
      }
    }

    // 3. 解析精度
    int prec = -1;
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (isdigit(*fmt)) {
          prec = prec * 10 + (*fmt - '0');
          fmt++;
        }
      }
    }

    // 4. 解析长度修饰: l计数为正, h计数为负
    int lenmod = 0;
    for (;; fmt++) {
      if (*fmt == 'l') {
        lenmod++;
      } else if (*fmt == 'h') {
        lenmod--;
      } else {
        break;
      }
    }

    // 5. 解析转换字符
    char conv = *fmt;
    switch (conv) {
      case 'd':
      case 'i': {
        long long v;
        if (lenmod >= 2) {
          v = va_arg(ap, long long);
        } else if (lenmod == 1) {
          v = va_arg(ap, long);
        } else if (lenmod < 0) { // h/hh: 先按int取出再截断
          v = (signed char)va_arg(ap, int);
        } else {
          v = va_arg(ap, int);
        }
        unsigned long long uv = (unsigned long long)v;
        int neg = v < 0;
        if (neg) {
          uv = -uv;
        }
        ob_num(o, uv, 10, 0, neg, width, prec, flags);
        break;
      }
      case 'u':
      case 'o':
      case 'x':
      case 'X': {
        unsigned long long v;
        if (lenmod >= 2) {
          v = va_arg(ap, unsigned long long);
        } else if (lenmod == 1) {
          v = va_arg(ap, unsigned long);
        } else {
          v = va_arg(ap, unsigned int);
        }
        int base = (conv == 'o') ? 8 : (conv == 'u') ? 10 : 16;
        ob_num(o, v, base, conv == 'X', 0, width, prec, flags);
        break;
      }
      case 'c': {
        ob_putc(o, (char)va_arg(ap, int));
        break;
      }
      case 's': {
        const char *s = va_arg(ap, const char *);
        if (s == NULL) {
          s = "(null)";
        }
        int slen = 0;
        while (s[slen] != '\0' && (prec < 0 || slen < prec)) {
          slen++; // 精度限制输出长度
        }
        for (int i = slen; i < width; i++) {
          ob_putc(o, ' ');
        }
        for (int i = 0; i < slen; i++) {
          ob_putc(o, s[i]);
        }
        break;
      }
      case 'p': {
        void *pv = va_arg(ap, void *);
        ob_puts(o, "0x");
        ob_num(o, (unsigned long long)(unsigned long)pv, 16, 0, 0, 0, -1, 0);
        break;
      }
      case 'f': {
        ob_double(o, va_arg(ap, double), width, prec, flags);
        break;
      }
      case '%': {
        ob_putc(o, '%');
        break;
      }
      case '\0': { // 格式串以%结尾, 结束
        return;
      }
      default: { // 未知转换, 原样输出
        ob_putc(o, '%');
        ob_putc(o, conv);
        break;
      }
    }
  }
}

static void ob_finish(struct obuf *o, char *out, size_t n) {
  if (out != NULL && n > 0) {
    size_t end = ((size_t)o->len < n - 1) ? (size_t)o->len : n - 1;
    out[end] = '\0';
  }
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
  struct obuf o = {.buf = out, .cap = n, .len = 0};
  fmt_core(&o, fmt, ap);
  ob_finish(&o, out, n);
  return o.len;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
  return vsnprintf(out, (size_t)-1, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) {
  struct obuf o = {.buf = NULL, .cap = 0, .len = 0}; // 直接流式输出到putch
  fmt_core(&o, fmt, ap);
  return o.len;
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vprintf(fmt, ap);
  va_end(ap);
  return ret;
}

int sprintf(char *out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsprintf(out, fmt, ap);
  va_end(ap);
  return ret;
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return ret;
}

int __am_vsscanf_internal(const char *str, const char **end_pstr, const char *fmt, va_list ap) {
  const char *pstr = str;
  const char *pfmt = fmt;
  int item = -1;
  while (*pfmt) {
    char ch = *pfmt ++;
    if (isspace(ch)) {
      for (ch = *pfmt; isspace(ch); ch = *(++ pfmt));
      for (ch = *pstr; isspace(ch); ch = *(++ pstr));
      item ++;
      continue;
    }
    switch (ch) {
      case '%': break;
      default:
        if (*pstr == ch) { // match
          pstr ++;
          item ++;
          continue;
        }
        goto end; // fail
    }

    char *p;
    ch = *pfmt ++;
    switch (ch) {
      // conversion specifier
      case 'd':
        *(va_arg(ap, int *)) = strtol(pstr, &p, 10);
        if (p == pstr) goto end; // fail
        pstr = p;
        item ++;
        break;

      case 'c':
        *(va_arg(ap, char *)) = *pstr ++;
        item ++;
        break;

      default:
        printf("Unsupported conversion specifier '%c'\n", ch);
        assert(0);
    }
  }

end:
  if (end_pstr) {
    *end_pstr = pstr;
  }
  return item;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
  return __am_vsscanf_internal(str, NULL, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsscanf(str, fmt, ap);
  va_end(ap);
  return r;
}

int __isoc99_sscanf(const char *str, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsscanf(str, fmt, ap);
  va_end(ap);
  return r;
}

#endif
