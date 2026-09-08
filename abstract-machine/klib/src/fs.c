#include <klib.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

// ==================== 基于ramdisk的简易文件系统 ====================
// gen-ramdisk.py 把 diskfilelist 里的文件嵌入镜像(.rodata.disk), 并生成
// struct file数组 __am_filelist. 这里的 fopen/fread 等API就是把文件操作
// 映射到这段内存数据上: fopen查表, 其余操作只维护一个偏移量.

// 与 gen-ramdisk.py 生成的结构体保持一致
struct file {
  const char *name;
  uint8_t *base;
  uint32_t size;
};
extern struct file *__am_filelist;

struct _IO_FILE {
  struct file *f;    // 指向ramdisk中的文件, NULL表示未打开
  uint32_t offset;   // 当前读写位置
};

// 与工具链stdio.h中FILE的底层类型同名, 供本文件内部使用
typedef struct _IO_FILE FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static struct _IO_FILE stdout_file; // stdout: printf直接走putch, 这里只是占位

FILE *stdout = &stdout_file;

FILE *fopen(const char *path, const char *mode) {
  for (struct file *f = __am_filelist; f->name != NULL; f ++) {
    if (strcmp(f->name, path) == 0) {
      FILE *fp = malloc(sizeof(FILE));
      fp->f = f;
      fp->offset = 0;
      return fp;
    }
  }
  return NULL;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
  assert(fp != NULL && fp->f != NULL);
  uint32_t remain = fp->f->size - fp->offset;
  uint32_t want = size * nmemb;
  uint32_t n = (want <= remain ? want : remain);
  memcpy(ptr, fp->f->base + fp->offset, n);
  fp->offset += n;
  return n / size; // 返回完整读到的条目数, 不足一条的部分丢弃(与标准语义一致)
}

int fseek(FILE *fp, long offset, int whence) {
  assert(fp != NULL && fp->f != NULL);
  long new_off = fp->offset;
  if (whence == SEEK_SET) new_off = offset;
  else if (whence == SEEK_CUR) new_off += offset;
  else new_off = (long)fp->f->size + offset;
  if (new_off < 0 || new_off > (long)fp->f->size) return -1;
  fp->offset = (uint32_t)new_off;
  return 0;
}

long ftell(FILE *fp) {
  assert(fp != NULL && fp->f != NULL);
  return (long)fp->offset;
}

int fclose(FILE *fp) {
  if (fp == NULL || fp == stdout) return 0;
  free(fp);
  return 0;
}

int fflush(FILE *fp) {
  return 0; // 无缓冲, 直接写穿
}

#endif
