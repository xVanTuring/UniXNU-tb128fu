// SPDX-License-Identifier: MIT
// TB128FU log reader init (see build-log-reader.py). Copies the UniXNU diagnostic log window, the last 32 MiB of userdata
// (package/BootLog.c with sheng-log=1), into LOG.BIN of the FAT image /log.img and offers that image read only as a USB mass
// storage disk; STATUS.TXT in the same image says what this program did. When the host ejects the disk, or after TIMEOUT_S, it
// restarts into fastboot. The stock TB128FU kernel has no USB serial gadget (the sheng reader's transport), but mass storage
// needs only the host's own drivers. Static and libc-free; nothing on UFS is written.
// With /vendor.list in the initramfs (build-log-reader.py --vendor-file), it also copies files of the stock vendor partition
// (the touch controller's firmware) into FIRMWARE.BIN: the partition's extent from the super partition's metadata, a read-only
// loop device over that range and a read-only mount.

#define LOG_BLOCKS 8192
#define BLOCK      4096
#define CHUNK      (1024 * 1024)
#define TIMEOUT_S  300
#define GADGET     "/sys/kernel/config/usb_gadget/log"
#define LUN        GADGET "/functions/mass_storage.0/lun.0"

enum {
  SysIoctl = 29, SysMknodat = 33, SysMkdirat = 34, SysSymlinkat = 36, SysUmount2 = 39, SysMount = 40, SysOpenat = 56, SysClose = 57,
  SysGetdents64 = 61, SysRead = 63, SysWrite = 64, SysPread64 = 67, SysPwrite64 = 68, SysSync = 81, SysExit = 93, SysNanosleep = 101,
  SysReboot = 142,
};
enum { AtFdcwd = -100, ORdonly = 0, OWronly = 1, ORdwr = 2, MsRdonly = 1 };
// linux/loop.h
enum { LoopSetFd = 0x4C00, LoopClrFd = 0x4C01, LoopSetStatus64 = 0x4C04, LoopCtlGetFree = 0x4C82 };

static long sys(long n, long a, long b, long c, long d, long e)
{
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
                x3 __asm__("x3") = d, x4 __asm__("x4") = e;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
  return x0;
}

// The compiler may emit calls to these; the volatile stores keep it from turning the loops back into calls.
void *memset(void *d, int c, unsigned long n)
{
  volatile unsigned char *p = d;
  while (n--) *p++ = (unsigned char)c;
  return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
  volatile unsigned char *p = d;
  const unsigned char *q = s;
  while (n--) *p++ = *q++;
  return d;
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) ++n; return n; }

// 0 if the first N bytes of A and B are equal.
static int memcmp_n(const void *a, const void *b, unsigned long n)
{
  const unsigned char *p = a, *q = b;
  for (unsigned long i = 0; i < n; ++i) if (p[i] != q[i]) return 1;
  return 0;
}

static int contains(const char *hay, unsigned long len, const char *needle)
{
  unsigned long n = slen(needle);
  for (unsigned long i = 0; i + n <= len; ++i) {
    unsigned long j = 0;
    while (j < n && hay[i + j] == needle[j]) ++j;
    if (j == n) return 1;
  }
  return 0;
}

static void sleep_s(long seconds) { long ts[2] = {seconds, 0}; sys(SysNanosleep, (long)ts, 0, 0, 0, 0); }

// STATUS.TXT: one line per step, rewritten into the image after every step.
static char status[4096];
static unsigned long status_len, status_off, status_cap;
static int image = -1;

static void put(const char *s)
{
  unsigned long n = slen(s);
  if (status_len + n < sizeof(status)) { memcpy(status + status_len, s, n); status_len += n; }
}
static void put_num(unsigned long v)
{
  char b[24]; int i = 23; b[i] = 0;
  do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
  put(b + i);
}
static void put_hex(unsigned long v)
{
  char b[24]; int i = 23; b[i] = 0;
  do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
  put("0x"); put(b + i);
}
static void flush_status(void)
{
  if (image < 0 || !status_cap) return;
  static char page[4096];
  unsigned long n = status_cap < sizeof(page) ? status_cap : sizeof(page);
  memset(page, ' ', n);
  memcpy(page, status, status_len < n ? status_len : n);
  page[n - 1] = '\n';
  sys(SysPwrite64, image, (long)page, (long)n, (long)status_off, 0);
}
static void step(const char *what, long result)
{
  put(what); put(" = "); if (result < 0) { put("-"); put_num((unsigned long)-result); } else put_num((unsigned long)result);
  put("\n"); flush_status();
}

static long write_file(const char *path, const char *text)
{
  long fd = sys(SysOpenat, AtFdcwd, (long)path, OWronly, 0, 0);
  if (fd < 0) return fd;
  long r = sys(SysWrite, fd, (long)text, (long)slen(text), 0, 0);
  sys(SysClose, fd, 0, 0, 0, 0);
  return r;
}
static long read_file(const char *path, char *buf, unsigned long cap)
{
  long fd = sys(SysOpenat, AtFdcwd, (long)path, ORdonly, 0, 0);
  if (fd < 0) return fd;
  long r = sys(SysRead, fd, (long)buf, (long)cap - 1, 0, 0);
  sys(SysClose, fd, 0, 0, 0, 0);
  buf[r > 0 ? r : 0] = 0;
  return r;
}
static unsigned long parse_num(const char **p)
{
  unsigned long v = 0;
  while (**p == ' ' || **p == '\n') ++*p;
  while (**p >= '0' && **p <= '9') v = v * 10 + (unsigned long)(*(*p)++ - '0');
  return v;
}

// The first entry of DIR (other than . and ..) whose name satisfies MATCH, copied into NAME; 1 if found.
static int scan_dir(const char *dir, int (*match)(const char *), char *name, unsigned long cap)
{
  static char ents[8192];
  long fd = sys(SysOpenat, AtFdcwd, (long)dir, ORdonly, 0, 0);
  if (fd < 0) return 0;
  int found = 0;
  long n;
  while (!found && (n = sys(SysGetdents64, fd, (long)ents, sizeof(ents), 0, 0)) > 0) {
    for (long at = 0; at < n && !found;) {
      unsigned short reclen = *(unsigned short *)(ents + at + 16);
      const char *entry = ents + at + 19;
      if (entry[0] != '.' && slen(entry) < cap && match(entry)) { memcpy(name, entry, slen(entry) + 1); found = 1; }
      at += reclen;
    }
  }
  sys(SysClose, fd, 0, 0, 0, 0);
  return found;
}
// The partition is_partition() looks for (its GPT name).
static const char *partition_name;
static int is_partition(const char *entry)
{
  char path[128] = "/sys/class/block/", buf[512], needle[64] = "\nPARTNAME=";
  unsigned long n = slen(path), m = slen(entry), k = slen(needle), w = slen(partition_name);
  if (n + m + 8 > sizeof(path) || k + w + 2 > sizeof(needle)) return 0;
  memcpy(path + n, entry, m); memcpy(path + n + m, "/uevent", 8);
  memcpy(needle + k, partition_name, w); memcpy(needle + k + w, "\n", 2);
  long r = read_file(path, buf, sizeof(buf));
  return r > 0 && contains(buf, (unsigned long)r, needle);
}
static int any_entry(const char *entry) { (void)entry; return 1; }

static void join(char *out, const char *a, const char *b)
{
  unsigned long n = slen(a), m = slen(b);
  memcpy(out, a, n); memcpy(out + n, b, m + 1);
}

// Opens /dev/NAME (a block device of /sys/class/block) with FLAGS. The stock kernel has no devtmpfs: the node is made from the
// device's major:minor in sysfs; TYPE is the node's file type (060000 block, 020000 character).
static long open_node(const char *name, long flags, long type)
{
  char path[160], sysdev[160], devno[32];
  join(path, "/dev/", name);
  long fd = sys(SysOpenat, AtFdcwd, (long)path, flags, 0, 0);
  if (fd >= 0) return fd;
  join(sysdev, "/sys/class/block/", name); join(sysdev + slen(sysdev), "", "/dev");
  if (read_file(sysdev, devno, sizeof(devno)) <= 0) return -19;
  const char *q = devno;
  unsigned long major = parse_num(&q);
  ++q;
  unsigned long minor = parse_num(&q);
  put(name); put("_dev="); put_num(major); put(":"); put_num(minor); put("\n");
  long encoded = (long)((minor & 0xff) | (major << 8) | ((minor & ~0xffUL) << 12));
  step("mknod", sys(SysMknodat, AtFdcwd, (long)path, type | 0600, encoded, 0));
  return sys(SysOpenat, AtFdcwd, (long)path, flags, 0, 0);
}

// Opens the partition named NAME read only; its kernel name goes to PART.
static long open_partition(const char *name, char *part, unsigned long cap)
{
  int found = 0;
  partition_name = name;
  for (int i = 0; i < 30 && !found; ++i) {
    found = scan_dir("/sys/class/block", is_partition, part, cap);
    if (!found) sleep_s(1);
  }
  if (!found) return -2;
  put(name); put("=/dev/"); put(part); put("\n");
  return open_node(part, ORdonly, 060000);
}

static long copy_log(unsigned long log_off, unsigned long log_bytes)
{
  static char buf[CHUNK];
  char part[64], path[160];
  long dev = open_partition("userdata", part, sizeof(part));
  if (dev < 0) return dev;
  join(path, "/sys/class/block/", part); join(path + slen(path), "", "/size");
  char size[32];
  if (read_file(path, size, sizeof(size)) <= 0) { sys(SysClose, dev, 0, 0, 0, 0); return -5; }
  const char *p = size;
  unsigned long blocks = parse_num(&p) / (BLOCK / 512);
  put("userdata_blocks="); put_num(blocks); put("\n");
  if (blocks < LOG_BLOCKS || log_bytes != (unsigned long)LOG_BLOCKS * BLOCK) { sys(SysClose, dev, 0, 0, 0, 0); return -22; }
  unsigned long first = (blocks - LOG_BLOCKS) * BLOCK;
  put("log_first_byte="); put_hex(first); put("\n");
  for (unsigned long done = 0; done < log_bytes; done += CHUNK) {
    long r = sys(SysPread64, dev, (long)buf, CHUNK, (long)(first + done), 0);
    if (r != CHUNK) { sys(SysClose, dev, 0, 0, 0, 0); return r < 0 ? r : -5; }
    if (!done) { put("log_magic="); for (int i = 0; i < 8; ++i) put_hex((unsigned char)buf[i]), put(" "); put("\n"); }
    r = sys(SysPwrite64, image, (long)buf, CHUNK, (long)(log_off + done), 0);
    if (r != CHUNK) { sys(SysClose, dev, 0, 0, 0, 0); return r < 0 ? r : -5; }
  }
  sys(SysClose, dev, 0, 0, 0, 0);
  return (long)log_bytes;
}

static unsigned long rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned long)p[3] << 24; }
static unsigned long rd64(const unsigned char *p) { return rd32(p) | rd32(p + 4) << 32; }

// Android dynamic partitions (liblp, metadata_format.h): the geometry is at byte 4096 of super, slot 0's primary metadata at
// 4096 + 2 * 4096. Finds partition NAME and returns its byte offset and size in super if it is one linear extent on super
// itself; the checksums are not verified, the table bounds are.
static long find_extent(long super, const char *name, unsigned long *offset, unsigned long *bytes)
{
  static unsigned char meta[65536];
  if (sys(SysPread64, super, (long)meta, 4096, 4096, 0) != 4096) return -5;
  if (rd32(meta) != 0x616c4467) return -22;                 // LP_METADATA_GEOMETRY_MAGIC
  unsigned long max = rd32(meta + 40);                      // metadata_max_size
  put("lp_metadata_max="); put_num(max); put("\n");
  if (max < 256 || max > sizeof(meta)) return -27;
  if (sys(SysPread64, super, (long)meta, (long)max, 3 * 4096, 0) != (long)max) return -5;
  if (rd32(meta) != 0x414c5030) return -22;                 // LP_METADATA_HEADER_MAGIC
  unsigned long header = rd32(meta + 8), tables = rd32(meta + 44);
  unsigned long parts = rd32(meta + 80), part_count = rd32(meta + 84), part_size = rd32(meta + 88);
  unsigned long extents = rd32(meta + 92), extent_count = rd32(meta + 96), extent_size = rd32(meta + 100);
  put("lp_version="); put_num(rd32(meta + 4) & 0xffff); put("."); put_num(rd32(meta + 4) >> 16);
  put(" partitions="); put_num(part_count); put(" extents="); put_num(extent_count); put("\n");
  if (header + tables > max || part_size < 52 || extent_size < 24 || parts + part_count * part_size > tables ||
      extents + extent_count * extent_size > tables)
    return -22;
  for (unsigned long i = 0; i < part_count; ++i) {
    const unsigned char *e = meta + header + parts + i * part_size;
    unsigned long n = slen(name);
    if (n >= 36 || memcmp_n(e, name, n) || e[n]) continue;
    unsigned long first = rd32(e + 40), count = rd32(e + 44);
    put(name); put(" first_extent="); put_num(first); put(" extents="); put_num(count); put("\n");
    if (count != 1) return -95;
    if (first >= extent_count) return -22;
    const unsigned char *x = meta + header + extents + first * extent_size;
    unsigned long sectors = rd64(x), type = rd32(x + 8), target = rd64(x + 12), source = rd32(x + 20);
    put(name); put(" sectors="); put_num(sectors); put(" type="); put_num(type); put(" start="); put_num(target);
    put(" source="); put_num(source); put("\n");
    if (type != 0 || source != 0) return -95;               // LP_TARGET_TYPE_LINEAR on the super partition itself
    *offset = target * 512; *bytes = sectors * 512;
    return 0;
  }
  return -2;
}

// FIRMWARE.BIN: per file a 64-byte record header ("UXFILE01", the size or a negative errno as a signed 64-bit number, the
// path, zero padded), then the data padded to 8 bytes; "UXEND000" after the last.
static unsigned long fw_off, fw_cap, fw_at;
static long fw_record(const char *path, long result, long fd)
{
  static char buf[CHUNK];
  char head[64];
  unsigned long n = slen(path), at = fw_at + 64, size = 0;
  if (fw_at + 64 + 8 > fw_cap) return -27;
  while (result >= 0 && fd >= 0) {
    long r = sys(SysRead, fd, (long)buf, CHUNK, 0, 0);
    if (r <= 0) { if (r < 0) result = r; break; }
    if (at + (unsigned long)r + 8 > fw_cap) { result = -27; break; }
    sys(SysPwrite64, image, (long)buf, r, (long)(fw_off + at), 0);
    at += (unsigned long)r; size += (unsigned long)r;
  }
  if (result >= 0) result = (long)size;
  memset(head, 0, sizeof(head));
  memcpy(head, "UXFILE01", 8);
  for (int i = 0; i < 8; ++i) head[8 + i] = (char)((unsigned long)result >> (8 * i));
  memcpy(head + 16, path, n < 47 ? n : 47);
  sys(SysPwrite64, image, (long)head, 64, (long)(fw_off + fw_at), 0);
  fw_at = result >= 0 ? (at + 7) / 8 * 8 : fw_at + 64;
  sys(SysPwrite64, image, (long)"UXEND000", 8, (long)(fw_off + fw_at), 0);
  return result;
}

// Firmware names worth listing: the touch controllers' and the GPU's (SQE microcode, zap shader).
static int vendor_like(const char *entry)
{
  static const char *const names[] = {"imax", "ovatek", "sqe", "zap", "a6"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (contains(entry, slen(entry), names[i])) return 1;
  return 0;
}

// /vendor.list: the partition's name, then one path inside it per line.
static long copy_vendor(void)
{
  static char list[1024];
  long r = read_file("/vendor.list", list, sizeof(list));
  if (r <= 0) return 0;
  char *line = list, *end = list + r;
  while (line < end && *line != '\n') ++line;
  if (line >= end) return -22;
  *line++ = 0;
  char part[64];
  long super = open_partition("super", part, sizeof(part));
  if (super < 0) return super;
  unsigned long offset = 0, bytes = 0;
  r = find_extent(super, list, &offset, &bytes);
  step("lp.extent", r);
  if (r < 0) { sys(SysClose, super, 0, 0, 0, 0); return r; }
  long ctl = sys(SysOpenat, AtFdcwd, (long)"/dev/loop-control", ORdwr, 0, 0);
  if (ctl < 0) {
    step("mknod.loop-control", sys(SysMknodat, AtFdcwd, (long)"/dev/loop-control", 020600, (10 << 8) | 237, 0));
    ctl = sys(SysOpenat, AtFdcwd, (long)"/dev/loop-control", ORdwr, 0, 0);
  }
  step("loop-control", ctl);
  long number = ctl >= 0 ? sys(SysIoctl, ctl, LoopCtlGetFree, 0, 0, 0) : 0;
  step("loop.free", number);
  if (ctl >= 0) sys(SysClose, ctl, 0, 0, 0, 0);
  if (number < 0) { sys(SysClose, super, 0, 0, 0, 0); return number; }
  char loop[16] = "loop";
  { char digits[8]; int i = 7; digits[i] = 0; unsigned long v = (unsigned long)number;
    do { digits[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    join(loop + 4, "", digits + i); }
  long dev = open_node(loop, ORdonly, 060000);
  step("loop.open", dev);
  if (dev < 0) { sys(SysClose, super, 0, 0, 0, 0); return dev; }
  // super is open read only, so the loop device is read only (LO_FLAGS_READ_ONLY).
  r = sys(SysIoctl, dev, LoopSetFd, super, 0, 0);
  step("loop.set_fd", r);
  sys(SysClose, super, 0, 0, 0, 0);
  if (r < 0) { sys(SysClose, dev, 0, 0, 0, 0); return r; }
  unsigned long info[29];                                   // struct loop_info64, 232 bytes
  memset(info, 0, sizeof(info));
  info[3] = offset; info[4] = bytes;                        // lo_offset, lo_sizelimit
  r = sys(SysIoctl, dev, LoopSetStatus64, (long)info, 0, 0);
  step("loop.set_status", r);
  char source[24];
  join(source, "/dev/", loop);
  sys(SysMkdirat, AtFdcwd, (long)"/vendor", 0755, 0, 0);
  long mounted = r < 0 ? r : sys(SysMount, (long)source, (long)"/vendor", (long)"ext4", MsRdonly, 0);
  step("mount.ext4", mounted);
  if (mounted < 0 && r >= 0) { mounted = sys(SysMount, (long)source, (long)"/vendor", (long)"erofs", MsRdonly, 0); step("mount.erofs", mounted); }
  if (mounted >= 0) {
    // The touch firmware names this vendor partition holds, for a list that misses.
    static char ents[8192];
    long dir = sys(SysOpenat, AtFdcwd, (long)"/vendor/firmware", ORdonly, 0, 0);
    long got = dir >= 0 ? sys(SysGetdents64, dir, (long)ents, sizeof(ents), 0, 0) : dir;
    put("vendor/firmware:");
    for (long at = 0; at < got;) {
      const char *entry = ents + at + 19;
      if (vendor_like(entry)) { put(" "); put(entry); }
      at += *(unsigned short *)(ents + at + 16);
    }
    put("\n");
    if (dir >= 0) sys(SysClose, dir, 0, 0, 0, 0);
    for (char *path = line; path < end;) {
      char *next = path;
      while (next < end && *next != '\n') ++next;
      if (next >= end) break;
      *next = 0;
      if (*path) {
        char full[160];
        join(full, "/vendor/", path);
        long fd = sys(SysOpenat, AtFdcwd, (long)full, ORdonly, 0, 0);
        step(path, fw_record(path, fd < 0 ? fd : 0, fd));
        if (fd >= 0) sys(SysClose, fd, 0, 0, 0, 0);
      }
      path = next + 1;
    }
    step("umount", sys(SysUmount2, (long)"/vendor", 0, 0, 0, 0));
  }
  step("loop.clear", sys(SysIoctl, dev, LoopClrFd, 0, 0, 0));
  sys(SysClose, dev, 0, 0, 0, 0);
  return mounted < 0 ? mounted : (long)fw_at;
}

static long gadget(void)
{
  static const char *const dirs[] = {GADGET, GADGET "/strings/0x409", GADGET "/configs/c.1",
                                     GADGET "/configs/c.1/strings/0x409", GADGET "/functions/mass_storage.0"};
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
    long r = sys(SysMkdirat, AtFdcwd, (long)dirs[i], 0755, 0, 0);
    if (r < 0 && r != -17) return r;
  }
  write_file(GADGET "/idVendor", "0x1d6b");
  write_file(GADGET "/idProduct", "0x0104");
  write_file(GADGET "/strings/0x409/manufacturer", "UniXNU");
  write_file(GADGET "/strings/0x409/product", "tb128fu log reader");
  write_file(GADGET "/strings/0x409/serialnumber", "unixnulog");
  write_file(GADGET "/configs/c.1/strings/0x409/configuration", "log");
  // ro and removable before file: the attributes cannot change while a backing file is open.
  step("lun.removable", write_file(LUN "/removable", "1"));
  step("lun.ro", write_file(LUN "/ro", "1"));
  step("lun.file", write_file(LUN "/file", "/log.img"));
  long r = sys(SysSymlinkat, (long)(GADGET "/functions/mass_storage.0"), AtFdcwd, (long)(GADGET "/configs/c.1/mass_storage.0"), 0, 0);
  step("config.link", r);
  if (r < 0 && r != -17) return r;
  char udc[64];
  int found = 0;
  for (int i = 0; i < 20 && !found; ++i) { found = scan_dir("/sys/class/udc", any_entry, udc, sizeof(udc)); if (!found) sleep_s(1); }
  if (!found) return -19;
  put("udc="); put(udc); put("\n");
  r = write_file(GADGET "/UDC", udc);
  step("udc.bind", r);
  // dwc3-msm leaves peripheral mode to the charger's VBUS report; a forced mode covers a cable already present at boot.
  step("ssusb.mode", write_file("/sys/bus/platform/devices/4e00000.ssusb/mode", "peripheral"));
  return r;
}

static void reboot_bootloader(void)
{
  sys(SysSync, 0, 0, 0, 0, 0);
  sys(SysReboot, (long)0xfee1dead, 672274793, (long)0xa1b2c3d4, (long)"bootloader", 0);
}

__attribute__((noreturn)) void start(void)
{
  sys(SysMkdirat, AtFdcwd, (long)"/dev", 0755, 0, 0);
  sys(SysMkdirat, AtFdcwd, (long)"/sys", 0755, 0, 0);
  long devfs = sys(SysMount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
  long sysfs = sys(SysMount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
  long cfs = sys(SysMount, (long)"configfs", (long)"/sys/kernel/config", (long)"configfs", 0, 0);

  // /log.offsets (build-log-reader.py): byte offset and size of LOG.BIN, then of STATUS.TXT and, with --vendor-file, of
  // FIRMWARE.BIN inside /log.img.
  char spec[128];
  read_file("/log.offsets", spec, sizeof(spec));
  const char *p = spec;
  unsigned long log_off = parse_num(&p), log_bytes = parse_num(&p);
  status_off = parse_num(&p); status_cap = parse_num(&p);
  fw_off = parse_num(&p); fw_cap = parse_num(&p);
  image = (int)sys(SysOpenat, AtFdcwd, (long)"/log.img", ORdwr, 0, 0);

  char release[160];
  put("tb128fu log reader\n");
  if (read_file("/proc/version", release, sizeof(release)) <= 0) {
    sys(SysMkdirat, AtFdcwd, (long)"/proc", 0755, 0, 0);
    sys(SysMount, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
    if (read_file("/proc/version", release, sizeof(release)) > 0) put(release);
  } else put(release);
  if (status_len && status[status_len - 1] != '\n') put("\n");
  step("mount.devtmpfs", devfs);
  step("mount.sysfs", sysfs);
  step("mount.configfs", cfs);
  step("image.open", image);
  step("log.copy", copy_log(log_off, log_bytes));
  if (fw_cap) step("vendor.copy", copy_vendor());
  step("gadget", gadget());

  char file[128];
  long waited = 0;
  for (; waited < TIMEOUT_S; ++waited) {
    sleep_s(1);
    long n = read_file(LUN "/file", file, sizeof(file));
    if (n >= 0 && (n == 0 || file[0] == '\n')) break;   // the host ejected the medium
  }
  put(waited < TIMEOUT_S ? "ejected by the host" : "timeout"); put("; restarting into fastboot\n");
  flush_status();
  reboot_bootloader();
  for (;;) sys(SysExit, 1, 0, 0, 0, 0);
}
