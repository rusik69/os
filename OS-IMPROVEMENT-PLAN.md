# OS Improvement Plan: 1000 Items

**Goal:** Systematically improve the rusik69/os hobby kernel across 12 phases — bug fixes, documentation, userspace commands, testing, code quality, kernel features, drivers, build system, security, performance, CI/CD, and final polish.

**Architecture:** Each item is self-contained and implementable in 2-10 minutes. One item per cron cycle. Build verification (-Wall -Wextra -Werror) before every commit.

**Build:** `make -j$(nproc) CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld OBJCOPY=x86_64-linux-gnu-objcopy` — zero warnings required.

---

## P01: Bug Hunt Continuation (items 1-120)

Continuing the existing bug-hunt from item 758 forward — driver, filesystem, and kernel correctness fixes.

1. FAT32: Add buffer overflow check when reading long file name entries (dir_entry + lfn_entry spans past sector boundary)
2. FAT32: Validate directory entry first byte is 0x00/E5/valid ASCII before treating as entry start
3. FAT32: Check cluster chain length against file size in fat32_read — return partial read on truncation
4. FAT32: Add sanity check on FAT cluster value before using as LBA (cluster < 2 or cluster > max_cluster)
5. FAT32: Validate FSInfo signature bytes before reading free cluster count
6. FAT32: Fix off-by-one in dir_find_by_path when path is root "/"
7. FAT32: Handle "." and ".." directory entries in vfat_reconstruct_name
8. FAT32: Add upper bound check in dir_remove_entry when scanning LFNs
9. FAT32: Validate lfn_ordinal sequence correctness (expects 1,2,3...LAST)
10. FAT32: Check for NULL fat32_fs_t pointer in fat32_read before dereference
11. ATA: Validate LBA range in ata_read before issuing command
12. ATA: Add timeout recovery in ata_pio_read_sector — retry on timeout
13. ATA: Check identify data buffer for valid signature before parsing
14. ATA: Validate sector count in multi-sector PIO transfers
15. PCI: Add vendor/device ID validation before config space access
16. PCI: Check bus number range in pci_find_device
17. PCI: Validate BAR address alignment in pci_read_bar
18. E1000: Validate ring descriptor pointers before DMA access
19. E1000: Add tx ring full handling — return EAGAIN instead of overrun
20. RTL8139: Validate packet buffer length against rx ring size
21. NVMe: Validate submission queue doorbell before writing
22. NVMe: Check completion queue entry phase tag for validity
23. USB EHCI: Validate qTD pointer alignment (must be 32-byte aligned)
24. USB EHCI: Add periodic frame list bounds check
25. USB MSC: Validate CSW signature after command transport
26. USB HID: Validate report descriptor length before parsing
27. USB HUB: Validate port status change bitmap against port count
28. Virtio-blk: Validate descriptor chain length before submitting
29. Virtio-net: Check available ring index wraps correctly
30. Virtio-pci: Validate device capability offsets before reading
31. AC97: Reset codec register cache on init
32. Sound PCM: Validate sample rate against codec capabilities
33. Sound mixer: Validate channel index in mixer control
34. PS/2 mouse: Validate packet byte sequence (expect 3-byte/4-byte)
35. PS/2 keyboard: Add scancode-to-keycode validation
36. RTC: Validate CMOS register access with NMI-disable pattern
37. CMOS: Validate century register value (2000+ adjust)
38. ACPI: Validate RSDP signature and checksum before parsing
39. ACPI: Validate DSDT length before table copy
40. ACPI EC: Add command timeout in ec_read/ec_write
41. ACPI thermal: Validate temperature sensor index
42. HPET: Validate timer comparator before programming
43. I2C: Validate slave address (7-bit vs 10-bit) before transaction
44. SMBUS: Validate block count before reading block data
45. Watchdog: Validate timeout value against hardware limits
46. I6300ESB: Validate timer register programming sequence
47. IPMI KCS: Validate state machine transitions in kcs_write
48. TPM TIS: Validate locality number before accessing registers
49. DRM core: Validate connector types before mode setting
50. BOCHS DRM: Validate framebuffer address before mmio
51. PVpanic: Validate event ID against known panic event list
52. VMWare balloon: Validate stats descriptor before reading
53. 9pnet: Validate message tag order in response handling
54. DM-crypt: Validate IV generation for each block (no IV reuse)
55. DM-verity: Validate hash tree root before verification
56. DM-raid: Validate superblock offset before reading
57. MD: Validate RAID superblock magic number
58. DRBD: Validate protocol version before connecting
59. NBD: Validate request handle uniqueness across inflight requests
60. ISCSI: Validate PDU length against buffer before parsing
61. NVMf: Validate connect command parameters before association
62. FCoE: Validate FCoE frame CRC before forwarding
63. Bonding: Validate slave link status before active-backup switch
64. VLAN: Validate VLAN tag before stripping/inserting
65. Bridge: Validate ageing timer before expiring FDB entry
66. MACsec: Validate ICV length before replay check
67. WireGuard: Validate handshake message nonce for replay protection
68. IPSec: Validate SA lifetime before accepting packets
69. IPVS: Validate connection tracking table size
70. NFtables: Validate rule expression chain length
71. Socket: Validate socket option level before dispatch
72. TCP: Validate MSS option value (minimum 536)
73. TCP: Validate window scale option value (max 14)
74. UDP: Validate checksum before delivering packet
75. IPv6: Validate extension header chain length (max 256)
76. IPv6 NDISC: Validate neighbor advertisement target address
77. ARP: Validate sender hardware address length in ARP replies
78. ICMP: Validate ICMP type before response generation
79. DHCP: Validate DHCP option length before parsing
80. DNS: Validate DNS response header ID matches query ID
81. HTTPD: Validate Content-Length header value before body read
82. Telnetd: Validate option negotiation sequence
83. SSHD: Validate key exchange packet length before parsing
84. TLS: Validate record length in TLS handshake
85. GDT: Validate segment selector index in gdt_set_entry
86. IDT: Validate interrupt vector number (0-255) in idt_set_entry
87. Page allocator: Validate order in buddy allocator free (0-10)
88. Page allocator: Validate zone index in page allocation
89. Slab allocator: Validate cache size against allocation request
90. VMM: Validate page table entry flags before setting
91. VMM: Validate virtual address range for user-space access
92. Heap: Validate allocation size in kmalloc (no overflow)
93. Process: Validate pid in process lookup (0-PID_MAX)
94. Process: Validate signal number (1-31) in signal delivery
95. Scheduler: Validate time slice in schedule_tick (non-zero)
96. Mutex: Validate owner thread id in mutex_unlock
97. Semaphore: Validate count in semaphore_wait (non-negative)
98. Pipe: Validate buffer size in pipe_create (minimum 4096)
99. Eventfd: Validate initval in eventfd_create (non-negative)
100. Inotify: Validate watch descriptor in inotify_rm_watch
101. Signal: Validate sigaction flags (SA_* mask) in sys_sigaction
102. Waitqueue: Validate condition function pointer before call
103. AIO: Validate iocb command type in aio_submit
104. Epoll: Validate epoll event mask in epoll_ctl
105. Timerfd: Validate timer fd flags in timerfd_create
106. Keyring: Validate key type in keyring_search
107. Audit: Validate audit record type before formatting
108. Seccomp: Validate seccomp filter instruction count before loading
109. NMI watchdog: Validate perf counter configuration
110. Ftrace: Validate function name length before registration
111. Kprobe: Validate probe address (must be in kernel text)
112. BPF: Validate eBPF program instruction count (max 4096)
113. Cgroup: Validate cgroup subsystem id in cgroup_attach
114. Namespace: Validate namespace type (CLONE_NEW*) in unshare
115. Devtmpfs: Validate device number range in device_create
116. Aio enhanced: Validate iocb data pointer before copy_from_user
117. Overlay: Validate lower layer path length before copy
118. IMA: Validate hash algorithm for appraisal
119. EVM: Validate xattr name in evm_verifyxattr
120. Landlock: Validate rule access mask in landlock_add_rule

---

## P02: Documentation (items 121-250)

Add/improve documentation across every subsystem. Focus on header docs, ARCHITECTURE, and inline function comments.

121. src/include: Add top-level kernel architecture overview comment to kernel.h
122. src/include: Document type definitions in types.h with usage notes
123. src/include: Add syscall table documentation to syscall.h
124. src/boot: Add boot flow documentation to boot.asm (multiboot header, GDT setup, mode switch)
125. src/kernel/kernel.c: Add module init ordering documentation
126. src/kernel/gdt.c: Document GDT layout (TSS, segment selectors, DPL levels)
127. src/kernel/idt.c: Document IDT entry layout and ISR wiring
128. src/kernel/fault.c: Document exception handler categories
129. src/kernel/syscall.c: Document syscall dispatch table with all entries
130. src/kernel/process.c: Document process lifecycle states
131. src/kernel/scheduler.c: Document scheduling policy (CFS-like? Round-robin?)
132. src/memory/pmm.c: Document physical memory allocator design
133. src/memory/vmm.c: Document virtual memory layout (user vs kernel space split)
134. src/memory/heap.c: Document kernel heap allocator
135. src/memory/slab.c: Document slab allocator cache structure
136. src/drivers/pci.c: Document PCI config space access API
137. src/drivers/vga.c: Document VGA text/framebuffer modes
138. src/drivers/timer.c: Document PIT/HPET timer setup
139. src/drivers/keyboard.c: Document scancode translation tables
140. src/drivers/serial.c: Document UART register programming
141. src/drivers/ata.c: Document ATA PIO/DMA transfer model
142. src/drivers/ahci.c: Document AHCI HBA register layout
143. src/drivers/nvme.c: Document NVMe queue pair model
144. src/drivers/usb_core.c: Document USB host controller driver model
145. src/drivers/e1000.c: Document e1000 register layout and descriptor rings
146. src/drivers/rtl8139.c: Document RTL8139 C+ mode operation
147. src/drivers/virtio_net.c: Document virtio-net descriptor layout
148. src/drivers/virtio_blk.c: Document virtio-blk request format
149. src/drivers/ac97.c: Document AC97 codec register map
150. src/fs/fat32.c: Document FAT32 on-disk structures
151. src/fs/fat32.h: Add struct field documentation for fat32_fs_t, fat32_dentry_t
152. src/fs/ext2.c: Document ext2 inode and block group descriptor layout
153. src/fs/ext4.c: Document ext4 extent tree structure
154. src/fs/iso9660.c: Document ISO9660 PVD and directory record format
155. src/fs/tmpfs.c: Document tmpfs inode and page management
156. src/fs/procfs.c: Document procfs file operations
157. src/fs/devfs.c: Document devfs device node management
158. src/fs/bufcache.c: Document buffer cache LRU/eviction policy
159. src/fs/page_cache.c: Document page cache readahead strategy
160. src/fs/vfs.h: Document VFS inode/dentry/superblock ops structs
161. src/fs/fsck.c: Document filesystem check algorithm
162. src/fs/luks.c: Document LUKS header parsing and key derivation
163. src/net/net.c: Document network stack layering
164. src/net/net_tcp.c: Document TCP state machine
165. src/net/net_udp.c: Document UDP socket handling
166. src/net/ipv6.c: Document IPv6 addressing and neighbor discovery
167. src/net/socket.c: Document socket API dispatch
168. src/net/netfilter.c: Document netfilter hook chain model
169. src/net/httpd.c: Document HTTP request parsing and routing
170. src/net/sshd.c: Document SSH protocol state machine
171. src/ipc/pipe.c: Document pipe ring buffer implementation
172. src/ipc/mutex.c: Document mutex/internal lock design
173. src/ipc/shm.c: Document shared memory page mapping
174. src/ipc/semaphore.c: Document semaphore wait queue
175. src/process/signal.c: Document signal delivery and handler dispatch
176. src/process/scheduler.c: Document scheduler tick and context switch
177. src/lib/string.c: Document libc string function implementations
178. src/lib/printf.c: Document printf format support
179. src/lib/stdio.c: Document buffered I/O model
180. src/lib/pthread.c: Document pthread implementation limitations
181. src/lib/radix_tree.c: Document radix tree operations
182. src/lib/bitmap.c: Document bitmap manipulation API
183. src/lib/mempool.c: Document memory pool allocation strategy
184. src/lib/aes.c: Document AES encryption implementation
185. src/lib/sha256.c: Document SHA-256 algorithm implementation
186. src/lib/crc32.c: Document CRC32 table generation
187. src/test/kunit.c: Document KUnit test framework
188. src/container/runtime.c: Document container lifecycle stages
189. src/container/network.c: Document container network isolation
190. src/container/image.c: Document container image format
191. src/orch/manifest.c: Document orchestrator manifest format
192. src/orch/rbac.c: Document orchestrator RBAC model
193. src/boot/uefi_gop.c: Document UEFI GOP framebuffer setup
194. src/power/suspend.c: Document suspend/resume flow
195. userspace/init/init.c: Document init process stages
196. userspace/bin/sh.c: Document shell command parsing
197. userspace/gui/gui.c: Add top-level GUI framework documentation
198. userspace/gui/gui_draw.c: Document drawing primitives API
199. userspace/gui/gui_widgets.c: Document widget lifecycle
200. userspace/gui/gui_apps.c: Document app registration mechanism
201. userspace/kmods/doom/: Add module-level docstring to doom_task.c
202. userspace/libc/include/unistd.h: Document syscall wrappers
203. userspace/libc/include/stdio.h: Document stdio functions
204. userspace/libc/include/string.h: Document string function availability
205. userspace/libc/include/stdlib.h: Document stdlib implementation status
206. src/include/errno.h: Add documentation for each errno constant
207. src/include/fcntl.h: Document file creation flags
208. src/include/signal.h: Document signal number meanings
209. src/include/sys/stat.h: Document file type/mode constants
210. src/include/sys/mman.h: Document mmap flags and protection
211. src/include/netinet/in.h: Document socket address structures
212. ARCHITECTURE.md: Create top-level architecture overview document
213. ARCHITECTURE.md: Add kernel memory layout diagram (text, data, heap, stack regions)
214. ARCHITECTURE.md: Add boot sequence flow (BIOS→boot.asm→kernel.c→init)
215. ARCHITECTURE.md: Add syscall table documentation
216. ARCHITECTURE.md: Add driver model documentation
217. ARCHITECTURE.md: Add VFS layer documentation
218. ARCHITECTURE.md: Add network stack architecture
219. ARCHITECTURE.md: Add container/orchestrator architecture
220. ARCHITECTURE.md: Add module (kmod) system documentation
221. ARCHITECTURE.md: Add GUI subsystem architecture
222. README.md: Add build prerequisites section
223. README.md: Add quick start guide (build → run in QEMU)
224. README.md: Add testing instructions (unit tests, e2e tests)
225. README.md: Add directory structure overview with subsystem list
226. README.md: Add contribution guidelines
227. Makefile: Add help target descriptions for all phony targets
228. Makefile: Document environment variable overrides (CC, LD, etc.)
229. Dockerfile.ci: Document build stages and tool purpose
230. src/kernel/config_gz.c: Add documentation for compressed config format
231. src/kernel/module.c: Document module loading stages (ELF parse→relocate→init)
232. src/kernel/module_elf.c: Document module ELF section handling
233. src/kernel/module_signature.c: Document module signature verification
234. src/kernel/caps.c: Document kernel capability model
235. src/kernel/seccomp.c: Document seccomp filter architecture
236. src/kernel/ima.c: Document IMA measurement and appraisal
237. src/kernel/audit.c: Document audit log format
238. src/kernel/ftrace.c: Document ftrace function tracing
239. src/kernel/kprobes.c: Document kprobe mechanism
240. src/kernel/lockdep.c: Document lock dependency validator
241. src/kernel/rcu.c: Document RCU synchronization mechanism
242. src/kernel/workqueue.c: Document workqueue threading model
243. src/kernel/kpti.c: Document KPTI (page table isolation) design
244. arch/ header files: Document any architecture-specific constants
245. src/drivers/fbcon.c: Document framebuffer console scrolling
246. src/drivers/drm/drm_core.c: Document DRM modesetting flow
247. src/power/cpufreq.c: Document CPU frequency scaling governors
248. src/power/devfreq.c: Document device frequency scaling
249. src/power/rapl.c: Document RAPL energy counter interface
250. src/power/cpuidle.c: Document CPU idle state hierarchy

---

## P03: New Userspace Commands (items 251-370)

Add missing Unix userspace commands that don't exist yet in /bin/.

251. mkfifoat: Add mkfifoat command (like mkfifo but with dirfd)
252. linkat: Add linkat command for creating hard links relative to dirfd
253. symlinkat: Add symlinkat command for symbolic links relative to dirfd
254. readlinkat: Add readlinkat wrapper for readlinkat syscall
255. fstatat: Add fstatat command for stat relative to dirfd
256. fdtdump: Add flattened device tree dump command
257. devicetree: Add device tree listing command
258. cpustat: Add per-CPU utilization stats command
259. memstat: Add detailed memory statistics (slab, pages, zones)
260. slabtop: Add slab allocator top-like display
261. iotop-userspace: Add per-process I/O stats command
262. fsusage: Add filesystem space usage by mount point
263. inotifywait: Add inotify event wait command
264. inotifywatch: Add inotify event watch/log command
265. acpi_listen: Add ACPI event listener command
266. powertop: Add power consumption estimation tool
267. turbostat: Add CPU frequency and turbo statistics
268. x86info: Add x86 CPU feature detection command
269. cpuid: Add CPUID instruction dump command
270. msr: Add MSR read/write command
271. dmidecode: Add DMI table decode command
272. lspci-verbose: Add -v (verbose) mode for lspci
273. lsusb-verbose: Add -v (verbose) mode for lsusb
274. lsblk-verbose: Add filesystem type detection to lsblk
275. blkdiscard-verbose: Add verbose mode to blkdiscard
276. fstrim-verbose: Add verbose/fstrim statistics
277. nvme-list: Add NVMe device list and namespace info
278. nvme-smart: Add NVMe SMART data retrieval
279. smartctl: Add ATA SMART data read command
280. hdparm: Add ATA device parameter read command
281. sdparm: Add SCSI device parameter command
282. lsscsi: Add SCSI device listing command
283. lsinitramfs: Add initramfs content listing command
284. lsmod-sort: Add sorted module listing (by size, by name)
285. modprobe-force: Add force mode to modprobe
286. lsmod-dep: Add module dependency tree display
287. depmod-generate: Add dependency file generation
288. sysctl-write: Add sysctl write/set functionality
289. sysctl-ignore: Add sysctl ignore-error mode
290. sysctl-pattern: Add sysctl pattern-based queries
291. kmod-sign: Add kernel module signing tool
292. kmod-verify: Add kernel module signature verification tool
293. kernel-boot-params: Add kernel boot parameter dump
294. fw_list: Add firmware loading listing command
295. lsirq: Add IRQ affinity listing command
296. irqbalance: Add simple one-shot IRQ balance command
297. perf-stat: Add perf event counting command
298. perf-record: Add perf event recording command
299. perf-top: Add perf top-like display
300. perf-list: Add perf event listing command
301. ftrace-tracer: Add ftrace enable/disable/read command
302. kprobe-add: Add kprobe registration command
303. kprobe-list: Add kprobe listing command
304. kprobe-rm: Add kprobe removal command
305. trace-cmd: Add trace event command
306. crash: Add kernel crash dump analysis command
307. vmcore-dmesg: Add vmcore dmesg extraction
308. kexec-load: Add kexec kernel loading frontend
309. kexec-exec: Add kexec execution command
310. reboot-mode: Add reboot mode (warm/cold) selection
311. shutdown-mode: Add shutdown mode (halt/poweroff) selection
312. ps-forest: Add ps --forest (tree view) mode
313. ps-threads: Add ps -T (thread view) mode
314. top-threads: Add top thread view mode (-H)
315. top-batch: Add top batch mode (-b)
316. killall: Add kill by process name command
317. pkill-uid: Add pkill -U (user filter) mode
318. pgrep-group: Add pgrep -G (group filter) mode
319. nice-adjust: Add nice value adjustment by pid
320. ionice-class: Add ionice class/idle mode
321. renice-group: Add renice by group (-g) mode
322. taskset-cpu: Add taskset CPU affinity by pid
323. chrt-deadline: Add chrt deadline scheduling mode
324. ulimit-all: Add ulimit -a (show all limits) mode
325. uname-all: Add uname -a (all info) mode
326. lscpu-cache: Add lscpu cache info display
327. lscpu-numa: Add lscpu NUMA node display
328. numactl: Add NUMA control command
329. getconf-all: Add getconf -a (all vars) mode
330. locale-gen: Add locale generation command
331. locale-lang: Add locale language listing
332. localedef-simple: Add simplified locale definition
333. dumpkeys: Add keyboard keymap dump command
334. loadkeys: Add keyboard keymap load command
335. setleds: Add keyboard LED control
336. kbdrate: Add keyboard repeat rate control
337. setfont: Add console font loading command
338. dpkg: Add minimal package manager (install/remove/list)
339. rpm: Add minimal RPM package manager
340. apt-sim: Add simulated apt install mode
341. pacman: Add minimal Arch-style package manager
342. portage: Add minimal Gentoo-style package manager
343. git-clone: Add minimal git clone (HTTP/SSH transport)
344. git-init: Add git init command
345. git-add: Add git add wrapper
346. git-status: Add git status wrapper
347. git-commit: Add git commit wrapper
348. git-log: Add git log wrapper
349. git-diff: Add git diff wrapper
350. hg: Add mercurial clone/status command
351. svn: Add subversion checkout command
352. make-cli: Add build system command wrapper
353. cmake-cli: Add cmake command wrapper
354. python3-cli: Add Python3 REPL wrapper
355. lua-cli: Add Lua interpreter wrapper
356. perl-cli: Add Perl one-liner wrapper
357. node-cli: Add Node.js REPL wrapper
358. ruby-cli: Add Ruby one-liner wrapper
359. whois: Add whois lookup command
360. nslookup-extended: Add extended DNS query options
361. dig: Add dig-style DNS lookup tool
362. host-cmd: Add host -t (type) record query
363. nmap-min: Add minimal port scanner command
364. traceroute: Add traceroute implementation
365. ping6: Add IPv6 ping command
366. mtr: Add my traceroute (ping+traceroute) command
367. netcat-udp: Add netcat UDP mode (-u)
368. netcat-listen: Add netcat listen mode (-l)
369. socat: Add socat-like bidirectional data relay
370. curl-https: Add HTTPS support check to curl

---

## P04: Unit Tests & Testing (items 371-480)

Add/extend unit tests in tests/host_libc and tests/unit.

371. tests/unit/test_string.c: Add strcmp edge case tests (empty, null bytes)
372. tests/unit/test_string.c: Add memmove overlap tests
373. tests/unit/test_string.c: Add strncpy truncation tests
374. tests/unit/test_string.c: Add strncat bounds tests
375. tests/unit/test_string.c: Add memcmp with different lengths test
376. tests/unit/test_bitmap.c: Add bitmap_set_area overflow test
377. tests/unit/test_bitmap.c: Add bitmap_find_first_zero full-bitmap test
378. tests/unit/test_bitmap.c: Add bitmap_clear_range bounds test
379. tests/unit/test_fs.c: Add path canonicalization tests
380. tests/unit/test_fs.c: Add path join edge cases (trailing slash, double slash)
381. tests/unit/test_fs.c: Add file extension extraction tests
382. tests/unit/test_list.c: Add list_add_tail singly-linked test
383. tests/unit/test_list.c: Add list_destroy null test
384. tests/unit/test_list.c: Add list_find empty-list test
385. tests/unit/test_memory.c: Add heap_alloc zero-size test
386. tests/unit/test_memory.c: Add heap_alloc large allocation (fail) test
387. tests/unit/test_memory.c: Add heap_free null pointer test
388. tests/unit/test_socket.c: Add socket address parse tests
389. tests/unit/test_socket.c: Add IP address validation tests
390. tests/unit/test_socket.c: Add port validation (0-65535) tests
391. tests/host_libc/test_libc.c: Add printf format edge cases
392. tests/host_libc/test_libc.c: Add scanf parsing tests
393. tests/host_libc/test_libc.c: Add atoi/atol boundary tests
394. tests/host_libc/test_libc.c: Add strtol base validation tests
395. tests/host_libc/test_libc.c: Add sprintf buffer overflow test
396. tests/host_libc/test_crc16.c: Add CRC16 empty input test
397. tests/host_libc/test_crc16.c: Add CRC16 single-byte edge cases
398. tests/host_libc/test_crc32.c: Add CRC32 table-based verif test
399. tests/host_libc/test_base64.c: Add base64 encode/decode roundtrip
400. tests/host_libc/test_base64.c: Add base64 padding edge cases
401. tests/host_libc/test_sha256.c: Add SHA-256 empty input test
402. tests/host_libc/test_sha256.c: Add SHA-256 known-answer tests
403. tests/host_libc/test_sha256.c: Add SHA-256 streaming API test
404. tests/host_libc/test_aes.c: Add AES-128 encrypt/decrypt roundtrip
405. tests/host_libc/test_aes.c: Add AES key expansion test vectors
406. tests/host_libc/test_cipher.c: Add cipher mode (ECB/CBC) tests
407. tests/host_libc/test_mempool.c: Add pool exhaustion test
408. tests/host_libc/test_mempool.c: Add pool free-during-usage test
409. tests/host_libc/test_mempool.c: Add pool alignment test
410. tests/host_libc/test_hashtable.c: Add hash collision test
411. tests/host_libc/test_hashtable.c: Add hashtable resize test
412. tests/host_libc/test_hashtable.c: Add hashtable delete-nonexistent test
413. tests/host_libc/test_find_bit.c: Add find_first_bit zero-length test
414. tests/host_libc/test_find_bit.c: Add find_next_bit wrapping test
415. tests/host_libc/test_hweight.c: Add hweight edge case (0, all-1s)
416. tests/host_libc/test_div64.c: Add div64 overflow cases
417. tests/host_libc/test_time.c: Add time conversion edge cases
418. tests/host_libc/test_range.c: Add range merge and overlap tests
419. tests/host_libc/test_ratelimit.c: Add burst and throttle tests
420. tests/host_libc/test_refcount.c: Add refcount wrap and saturation tests
421. tests/host_libc/test_uuid.c: Add UUID generation uniqueness test
422. tests/host_libc/test_uid16.c: Add uid/gid conversion edge cases
423. tests/host_libc/test_sort_ext.c: Add sort stability test
424. tests/host_libc/test_sched_attr.c: Add sched attribute roundtrip test
425. tests/host_libc/test_rng_core.c: Add RNG output distribution test
426. tests/host_libc/test_module_alias.c: Add module alias matching tests
427. tests/host_libc/test_kptr_restrict.c: Add pointer restriction levels test
428. tests/host_libc/test_stackleak.c: Add stack erasure verification test
429. tests/host_libc/test_errno_ext.c: Add errno string mapping test
430. tests/host_libc/test_stdlib_ext.c: Add strndup/memdup tests
431. tests/host_libc/test_cmdline.c: Add cmdline parse edge cases
432. tests/host_libc/test_negative_path.c: Add ENOMEM error path tests
433. tests/host_libc/stubs.c: Add missing stub documentation
434. tests/host_libc/spinlock.h: Add unit test spinlock test
435. tests/e2e.sh: Add boot test with kernel panic detection
436. tests/e2e.sh: Add disk image integrity check before boot
437. tests/e2e.sh: Add network interface presence test
438. tests/e2e.sh: Add filesystem mount and file create test
439. tests/e2e.sh: Add signal delivery test (kill + handler)
440. tests/e2e.sh: Add multi-process fork/exec test
441. tests/e2e.sh: Add pipe IPC data transfer test
442. tests/e2e.sh: Add socket loopback data transfer test
443. tests/e2e.sh: Add syscall fuzz test (random syscall numbers)
444. tests/e2e.py: Add telnet session log parsing
445. tests/e2e.py: Add test timeout handling with diagnostics
446. tests/e2e.py: Add result XML output for CI integration
447. tests/run_tests.sh: Add per-test timeout
448. tests/run_tests.sh: Add test result summary at exit
449. tests/run_tests.sh: Add QEMU output capture failure dump
450. src/test/kunit.c: Add KUnit assertion message formatting
451. src/test/kunit_tests.c: Add scheduler edge case tests
452. src/test/kunit_tests.c: Add mutex lock/unlock stress test
453. src/test/kunit_memory.c: Add page allocator boundary tests
454. src/test/kunit_vmm.c: Add page table walk tests
455. src/test/kunit_slab.c: Add slab cache grow/shrink tests
456. src/test/kunit_pmm.c: Add physical page alloc fragmentation test
457. src/test/kunit_sched.c: Add priority inheritance test
458. src/test/kunit_security.c: Add capability check tests
459. src/test/kunit_vfs.c: Add VFS path resolution tests
460. src/test/kunit_net.c: Add socket create/bind/listen lifecycle test
461. src/test/kunit_errno.c: Add syscall error propagation test
462. src/test/kunit_container_ext.c: Add namespace isolation test
463. src/test/kunit_coverage.c: Add code coverage data test
464. src/test/kunit_usb.c: Add USB device enumeration test
465. src/test/kunit_tls.c: Add TLS handshake/record test
466. src/test/kunit_regression.c: Add regression detection framework
467. tests/boot_test.py: Add serial console wait-for-prompt pattern
468. tests/boot_test.py: Add QEMU exit code checking
469. tests/boot_test.py: Add kernel panic pattern detection
470. tests/boot_test.py: Add snapshot comparison mode
471. tests/host_libc/Makefile: Add `test` target with result aggregation
472. tests/host_libc/Makefile: Add `coverage` target with gcov
473. tests/unit/Makefile: Add parallel test execution support
474. tests/unit/Makefile: Add CFLAGS hardening (-fstack-protector)
475. tests/e2e.sh: Add cleanup of stale QEMU processes on timeout
476. tests/e2e.sh: Add KVM detection and enable if available
477. tests/run_tests.sh: Add test kernel argument parsing
478. tests/doom_fb.sh: Add framebuffer pixel color verification
479. tests/doom_fb.sh: Add test timeout safeguard
480. tests/run_tests.sh: Add initramfs presence check

---

## P05: Code Quality & Refactoring (items 481-600)

Clean up code — remove dead code, fix header hygiene, reduce duplication, unify coding style.

481. src/kernel/kernel.c: Remove orphaned comments referencing deleted features
482. src/kernel/gdt.c: Unify GDT entry building into helper macro
483. src/kernel/idt.c: Remove unused IDT entry type definitions
484. src/kernel/syscall.c: Group syscall handler definitions by subsystem
485. src/kernel/process.c: Extract process state transition to inline helper
486. src/kernel/module.c: Remove duplicated ELF section parsing
487. src/kernel/module_elf.c: Add bounds check on ELF section header access
488. src/kernel/kernel_pch.h: Remove unused precompiled header includes
489. src/drivers/pci.c: Factor out PCI config read/write helpers
490. src/drivers/ata.c: Consolidate PIO read/write into single function
491. src/drivers/ata_pio.c: Remove dead ATA timing code
492. src/drivers/e1000.c: Extract descriptor ring setup into shared helper
493. src/drivers/virtio_pci_modern.c: Unify capability parsing
494. src/drivers/usb_ehci.c: Remove redundant qTD setup logic
495. src/drivers/usb_core.c: Centralize USB transfer error handling
496. src/drivers/ac97.c: Remove unused codec register definitions
497. src/drivers/fbcon.c: Extract scroll and putchar into helpers
498. src/drivers/drm/drm_core.c: Remove unused mode list walk code
499. src/fs/fat32.c: Factor out cluster chain walk into reusable helper
500. src/fs/fat32.c: Replace magic numbers with named constants
501. src/fs/fat32_lfn.c: Unify LFN checksum calculation across callers
502. src/fs/ext2.c: Extract block group descriptor read helper
503. src/fs/ext4.c: Remove duplicated extent tree walk
504. src/fs/bufcache.c: Extract LRU list management helpers
505. src/fs/page_cache.c: Remove dead readahead code paths
506. src/fs/vfs.h: Remove unused VFS operation declarations
507. src/fs/devfs.c: Remove unused device class iteration
508. src/net/net.c: Consolidate packet alloc/free into helpers
509. src/net/net_tcp.c: Extract TCP state machine transitions
510. src/net/socket.c: Remove duplicated address family dispatch
511. src/net/ipv6.c: Consolidate NDISC option parsing
512. src/net/netfilter.c: Factor out rule chain traversal
513. src/net/httpd.c: Remove unused HTTP method handlers
514. src/net/sshd.c: Unify key exchange packet handlers
515. src/memory/pmm.c: Remove duplicated page free lists
516. src/memory/vmm.c: Extract page table walk into shared function
517. src/memory/slab.c: Consolidate slab allocation paths
518. src/memory/heap.c: Remove dead heap compaction code
519. src/process/signal.c: Extract signal mask handling helpers
520. src/process/scheduler.c: Remove unused scheduling classes
521. src/ipc/pipe.c: Remove duplicated pipe buffer management
522. src/ipc/mutex.c: Consolidate lock acquisition paths
523. src/ipc/semaphore.c: Extract wait queue management
524. src/ipc/shm.c: Remove unused shared page accounting
525. src/lib/string.c: Remove duplicated string copy implementations
526. src/lib/printf.c: Consolidate format specifier dispatch
527. src/lib/stdio.c: Remove unused buffered I/O state
528. src/lib/stdlib.c: Consolidate memory allocation wrappers
529. src/lib/bitmap.c: Remove unused bitmap shift operations
530. src/lib/radix_tree.c: Extract node allocation helper
531. src/lib/mempool.c: Remove dead pool debug statistic code
532. src/lib/aes.c: Consolidate key expansion lookup tables
533. src/lib/sha256.c: Remove unused SHA-224 code paths
534. src/lib/hmac.c: Extract HMAC block processing
535. src/lib/crc32.c: Remove unused CRC32 variants
536. src/boot/boot.asm: Remove unused GDT entries
537. src/boot/uefi_gop.c: Consolidate GOP mode enumeration
538. src/container/runtime.c: Remove duplicated container state code
539. src/container/config.c: Extract config parsing helpers
540. src/container/storage.c: Remove unused storage driver code
541. src/orch/metrics.c: Consolidate metric aggregation
542. src/orch/log_shipper.c: Remove unused log format handlers
543. src/power/suspend.c: Extract device suspend/resume helpers
544. src/power/cpufreq.c: Remove unused governor registration paths
545. src/power/cpuidle.c: Consolidate idle state selection
546. src/test/kunit.c: Remove unused test result formatting paths
547. src/kernel/audit.c: Consolidate audit record formatting
548. src/kernel/lockdep.c: Remove unused lock class tracking
549. src/kernel/workqueue.c: Extract worker pool management
550. src/kernel/ftrace.c: Remove unused function tracing modes
551. src/kernel/kprobes.c: Consolidate probe registration paths
552. src/kernel/kasan_light.c: Remove unused address sanitizer modes
553. src/kernel/stackleak.c: Extract stack erasure helper
554. src/kernel/seccomp.c: Remove unused seccomp filter modes
555. src/kernel/ima.c: Consolidate IMA policy evaluation
556. userspace/gui/gui.c: Remove duplicated widget iteration patterns
557. userspace/gui/gui_draw.c: Consolidate primitive drawing functions
558. userspace/gui/gui_widgets.c: Extract widget event dispatch helper
559. userspace/gui/gui_apps.c: Remove unused app registration paths
560. userspace/libc/string.c: Remove duplicated memcpy implementations
561. userspace/libc/stdio.c: Consolidate buffered I/O operations
562. userspace/bin/sh.c: Extract command dispatch table builder
563. userspace/init/init.c: Remove unused init stage handlers
564. userspace/kmods/doom/: Remove unused render paths
565. userspace/kmods/dos/: Remove unused DOS interrupt handlers
566. Makefile: Remove duplicate source file entries in C_SRCS
567. Makefile: Consolidate CFLAGS definitions
568. Makefile: Extract boilerplate BUILD_TIME/KVERSION into helper
569. userspace/Makefile: Consolidate ELF build pattern rules
570. tests/Makefiles: Remove duplicate test source lists
571. src/include: Add header include guards where missing
572. src/include: Remove unused type definitions
573. src/include: Standardize include guard naming convention
574. userspace/libc/include: Standardize include guard naming
575. userspace/libc/include: Remove unused macro definitions
576. All .c files: Remove trailing whitespace (already tracked by check-whitespace)
577. All .h files: Remove trailing whitespace
578. All .asm files: Remove trailing whitespace
579. Review all // comments vs /* */ — standardize on kernel style
580. Review function pointer typedef naming — standardize on _t suffix
581. Review struct typedef naming — standardize on _t suffix
582. Review enum definitions — standardize on ALL_CAPS values
583. Add const qualifiers to read-only function parameters
584. Add static qualifiers to internal functions (not in headers)
585. Remove unused function parameters (replace with (void)param or remove)
586. Fix implicit int return types (should be void for no-return functions)
587. Fix implicit function declarations (missing includes)
588. Add -Wmissing-prototypes and fix all violations
589. Add -Wmissing-declarations and fix all violations
590. Add -Wstrict-overflow=5 and fix all violations
591. Add -Wfloat-equal and fix violations (kernel has no float normally)
592. Add -Wundef for all Makefile conditional compilation paths
593. Review all TODO comments — implement or remove
594. Review all FIXME comments — implement or remove
595. Review all XXX comments — implement or remove
596. Review all HACK comments — implement proper solution
597. Remove dead code guarded by #if 0
598. Remove dead code guarded by #ifdef NOT_YET
599. Remove dead code guarded by comments "unused"
600. Add -Wunused-macros and remove/ifdef-0 unused macros

---

## P06: Kernel Features & Syscalls (items 601-700)

Add new syscalls, kernel features, and improve existing kernel APIs.

601. syscall: Add SYS_GETRANDOM wrapper for kernel RNG access
602. syscall: Add SYS_MEMFD_CREATE for anonymous file-backed memory
603. syscall: Add SYS_MEMBARRIER for memory barrier synchronization
604. syscall: Add SYS_COPY_FILE_RANGE for kernel-space file copy
605. syscall: Add SYS_NAME_TO_HANDLE_AT for file handle creation
606. syscall: Add SYS_OPEN_BY_HANDLE_AT for file handle resolution
607. syscall: Add SYS_CLOCK_ADJTIME for clock slew adjustment
608. syscall: Add SYS_CLOCK_GETRES for clock resolution query
609. syscall: Add SYS_TIMER_GETOVERRUN for timer overrun count
610. syscall: Add SYS_SIGNALFD for signal delivery via fd
611. syscall: Add SYS_EVENTFD2 for enhanced eventfd
612. syscall: Add SYS_PIDFD_SEND_SIGNAL for pidfd-based signal
613. syscall: Add SYS_PIDFD_OPEN (part of pidfd family)
614. syscall: Add SYS_FCHMODAT for fchmodat
615. syscall: Add SYS_FCHOWNAT for fchownat
616. syscall: Add SYS_LINKAT for linkat
617. syscall: Add SYS_SYMLINKAT for symlinkat
618. syscall: Add SYS_READLINKAT for readlinkat
619. syscall: Add SYS_FSTATAT for fstatat
620. syscall: Add SYS_MKNODAT for mknodat
621. syscall: Add SYS_UNLINKAT for unlinkat
622. syscall: Add SYS_RENAMEAT for renameat
623. syscall: Add SYS_MKDIRAT for mkdirat
624. syscall: Add SYS_RMDIRAT for rmdirat
625. syscall: Add SYS_UTIMENSAT for nanosecond timestamps
626. syscall: Add SYS_STATX for extended file stats
627. syscall: Add SYS_GETTID for thread ID query
628. syscall: Add SYS_TGKILL for thread-directed signal
629. syscall: Add SYS_TKILL for thread kill (deprecated but compat)
630. syscall: Add fcntl F_DUPFD_CLOEXEC for close-on-exec dup
631. syscall: Add fcntl F_ADD_SEALS (memfd sealing)
632. syscall: Add fcntl F_GET_SEALS (memfd seal query)
633. syscall: O_TMPFILE support in open syscall
634. syscall: Add SYS_CAPGET for capability query
635. syscall: Add SYS_CAPSET for capability set
636. syscall: Add SYS_PRLIMIT64 for per-process rlimit queries
637. syscall: Add SYS_GETCPU for CPU number query
638. syscall: Add SYS_SCHED_GETATTR for scheduler attribute query
639. syscall: Add SYS_SCHED_SETATTR for scheduler attribute set
640. syscall: Add SYS_SCHED_GET_PRIORITY_MAX for priority range
641. syscall: Add SYS_IOPRIO_GET for I/O priority query
642. syscall: Add SYS_IOPRIO_SET for I/O priority set
643. syscall: Add SYS_PROCESS_MRELEASE (madvise release)
644. syscall: Add SYS_PIDFD_OPEN (recheck and add to dispatch)
645. syscall: Add SYS_CLONE3 with extensible struct clone_args
646. syscall: Wire SYS_ACCEPT4 in socket dispatch (accept with flags)
647. syscall: Wire SYS_SENDMMSG in socket dispatch (scatter-gather send)
648. syscall: Wire SYS_RECVMMSG in socket dispatch (scatter-gather recv)
649. syscall: Add SYS_PREADV for vectored pread
650. syscall: Add SYS_PWRITEV for vectored pwrite
651. syscall: Add SYS_PREADV2 for vectored pread with offset
652. syscall: Add SYS_PWRITEV2 for vectored pwrite with offset
653. syscall: Add SYS_SPLICE for zero-copy pipe-to-pipe data transfer
654. syscall: Add SYS_TEE for pipe data duplication
655. syscall: Add SYS_VMSPLICE for user-to-pipe zero-copy
656. syscall: Add SYS_FALLOCATE for file space preallocation
657. syscall: Add SYS_FINIT_MODULE for loading module from fd
658. syscall: Add SYS_KEXEC_FILE_LOAD for loading kexec from fd
659. syscall: Add SYS_FW_CREATE for firmware loading from userspace
660. syscall: Add SYS_PIVOT_ROOT for container chroot pivot
661. syscall: Add SYS_MOVE_MOUNT for mount point relocation
662. syscall: Add SYS_OPEN_TREE for opening mount as file handle
663. syscall: Add SYS_FSCONFIG for filesystem configuration
664. syscall: Add SYS_FSMOUNT for filesystem mount
665. syscall: Add SYS_FSPICK for filesystem pick
666. syscall: Improve SYS_CLOCK_NANOSLEEP with absolute mode
667. syscall: Add SYS_SCHED_YIELD for voluntary yield
668. syscall: Add SYS_SCHED_RR_GET_INTERVAL for round-robin time slice
669. Kernel: Add RLIMIT_NOFILE enforcement in file descriptor allocation
670. Kernel: Add RLIMIT_NPROC enforcement in fork
671. Kernel: Add RLIMIT_STACK enforcement in mmap
672. Kernel: Add RLIMIT_AS enforcement in mmap
673. Kernel: Add RLIMIT_CORE enforcement in coredump
674. Kernel: Add RLIMIT_CPU enforcement via timer
675. Kernel: Add RLIMIT_MEMLOCK enforcement in mlock/mlockall
676. Kernel: Add file descriptor table lock for concurrent access
677. Kernel: Add process group session management (setsid)
678. Kernel: Add controlling terminal assignment
679. Kernel: Add /proc/self/maps virtual file
680. Kernel: Add /proc/self/status virtual file
681. Kernel: Add /proc/self/cmdline virtual file
682. Kernel: Add /proc/self/environ virtual file
683. Kernel: Add /proc/self/limits virtual file
684. Kernel: Add /proc/self/cwd symlink
685. Kernel: Add /proc/self/exe symlink
686. Kernel: Add /proc/self/root symlink
687. Kernel: Add /proc/self/fd directory
688. Kernel: Add /proc/self/fdinfo directory
689. Kernel: Add /proc/self/ns directory (namespace links)
690. Kernel: Add /proc/version with kernel version string
691. Kernel: Add /proc/uptime with system uptime
692. Kernel: Add /proc/loadavg with CPU load averages
693. Kernel: Add /proc/stat with cumulative CPU/system stats
694. Kernel: Add /proc/meminfo with enhanced memory breakdown
695. Kernel: Add /proc/cpuinfo with per-CPU feature flags
696. Kernel: Add /proc/devices with major number registry
697. Kernel: Add /proc/interrupts with IRQ statistics
698. Kernel: Add /proc/ioports with I/O port allocation
699. Kernel: Add /proc/iomem with physical memory map
700. Kernel: Add /proc/modules with loaded module list

---

## P07: Driver Improvements (items 701-790)

Add new hardware support, improve existing drivers, add missing features.

701. ATA: Add NCQ (Native Command Queuing) support
702. ATA: Add SATA FIS-based command transmission
703. AHCI: Add port multiplier support
704. AHCI: Add aggressive link power management (ALPM)
705. NVMe: Add multi-queue (multiple I/O SQ/CQ pairs)
706. NVMe: Add AER (Asynchronous Event Request) support
707. NVMe: Add sanitize operation support
708. NVMe: Add format NVM command support
709. NVMe: Add persistent memory region namespaces
710. USB EHCI: Add split-transaction isochronous support
711. USB XHCI: Add event ring segment table management
712. USB XHCI: Add isochronous transfer support
713. USB core: Add USB3 speed descriptor parsing
714. USB core: Add USB device configuration descriptors
715. USB HID: Add multi-touch report parsing
716. USB HID: Add consumer control (volume, media keys)
717. USB MSC: Add UAS (USB Attached SCSI) support
718. USB CDC-ACM: Add modem control line handling
719. USB CDC-Ether: Add statistics counters
720. USB video: Add frame buffer format negotiation
721. USB audio: Add isochronous streaming support
722. Virtio-net: Add multi-queue support
723. Virtio-net: Add checksum offload
724. Virtio-net: Add TSO/GSO segmentation offload
725. Virtio-blk: Add discard/write-zeroes support
726. Virtio-blk: Add multi-queue support
727. Virtio-scsi: Add device discovery and LUN enumeration
728. Virtio-console: Add multi-port support
729. Virtio-gpu: Add 3D context creation (virgl)
730. Virtio-input: Add multi-touch events
731. Virtio-balloon: Add free page hint reporting
732. Virtio-balloon: Add page poisoning reporting
733. Virtio-rng: Add entropy estimation
734. Virtio-fs: Add file system passthrough
735. Virtio-iommu: Add DMA remapping support
736. PCIe: Add AER (Advanced Error Reporting) handler
737. PCIe: Add DPC (Downstream Port Containment) handling
738. PCIe: Add PTM (Precision Time Measurement) support
739. PCI: Add SR-IOV VF enumeration and configuration
740. PCI: Add PCIe capability list parsing
741. PCI: Add MSI-X vector management
742. E1000: Add VLAN filtering via VFTA
743. E1000: Add RSS (Receive Side Scaling) support
744. E1000: Add hardware checksum offload
745. RTL8139: Add multi-descriptor ring support
746. VMXNet3: Add TSO offload
747. VMXNet3: Add RSS hash reporting
748. IGB: Add NIC reset recovery
749. I40E: Add VEB (Virtual Ethernet Bridge) support
750. MLX4: Add port enumeration and link status
751. AC97: Add surround sound channel support
752. AC97: Add sample rate conversion control
753. Sound core: Add OSS audio device model
754. Sound core: Add mixer device enumeration
755. Sound PCM: Add 24-bit sample format support
756. Sound PCM: Add non-interleaved access mode
757. DRM: Add connector hotplug detection
758. DRM: Add EDID block read and parsing
759. DRM: Add dumb buffer creation and mmap
760. DRM: Add atomic mode set commit
761. BOCHS DRM: Add preferred mode detection
762. SimpleFB DRM: Add multi-monitor support
763. PS/2: Add AUX (mouse) port detection
764. i8042: Add controller self-test
765. VGA: Add mode switching via VBE
766. VGA: Add 24-bit color depth support
767. HPET: Add periodic timer mode with interrupt
768. RTC: Add alarm interrupt support
769. CMOS: Add extended CMOS bank switching
770. I2C: Add repeated start condition support
771. I2C: Add SMBus block read/write
772. SMBus: Add ARA (Alert Response Address) handling
773. SPI: Add SPI mode configuration
774. GPIO: Add IRQ support for GPIO lines
775. GPIO: Add debounce configuration
776. Watchdog: Add pretimeout notification
777. IPMI: Add LAN interface support
778. IPMI: Add SEL (System Event Log) read
779. TPM: Add TPM2 command submission
780. TPM: Add NV index read/write
781. Firmware: Add EFI runtime variable services
782. Firmware: Add ACPI CPPC feedback counter read
783. Firmware: Add ACPI DSDT table override loading
784. Firmware: Add SMBIOS entry point scanning
785. ACPI fan: Add active cooling threshold config
786. ACPI EC: Add burst mode for faster transfers
787. ACPI thermal: Add critical trip point handler
788. ACPI power button: Add keycode to input system
789. SRIOV: Add VF MAC address configuration
790. PMem: Add persistent memory namespace region

---

## P08: Build & Tooling (items 791-840)

Improve the build system, scripts, and development tooling.

791. Makefile: Add `make clean-kernel` target for kernel-only cleanup
792. Makefile: Add `make clean-userspace` target for userspace-only cleanup
793. Makefile: Add `make rebuild` (clean + all) target
794. Makefile: Add `make ccache-stats` target
795. Makefile: Add `make distclean` (clean + remove build/ entirely)
796. Makefile: Add `make install` with DESTDIR support
797. Makefile: Add `make -j` auto-detection for nproc
798. Makefile: Add verbose mode with V=1
799. Makefile: Add `make format` target that applies clang-format in-place
800. Makefile: Add `make tidy` target (clang-tidy auto-fix)
801. Makefile: Add `make check` alias for `make verify`
802. Makefile: Add conditional ccache support (auto-detect installed)
803. Makefile: Add conditional distcc support for distributed builds
804. Makefile: Add `make git-hooks` target to install pre-commit hooks
805. Makefile: Add colorized output support
806. Makefile: Add `make -n` (dry-run) compatibility
807. Makefile: Add BUILD_TYPE variable (debug/release) for CFLAGS selection
808. Makefile: Add debug build with -DDEBUG and -Og
809. Makefile: Add release build with -O3 and LTO support
810. Makefile: Add LTO (Link-Time Optimization) mode
811. Makefile: Add PGO (Profile-Guided Optimization) support
812. Makefile: Add coverage build target with gcov/lcov
813. Makefile: Add address sanitizer build mode
814. Makefile: Add undefined behavior sanitizer build mode
815. Makefile: Add memory sanitizer build mode for userspace
816. Makefile: Add thread sanitizer build mode for userspace
817. Makefile: Integrate pvs-studio or similar static analyzer
818. Makefile: Integrate flawfinder for security analysis
819. userspace/Makefile: Add silent/verbose mode
820. userspace/Makefile: Add per-binary build target support
821. userspace/Makefile: Add `make list-commands` to enumerate available commands
822. scripts/mkfatimg.py: Add verbose mode for debugging
823. scripts/mkfatimg.py: Add FAT32 filesystem validation after write
824. scripts/mkfatimg.py: Add initramfs integration option
825. scripts/mkfatimg.py: Add disk image size auto-calculation
826. scripts/mkfatimg.py: Add multi-partition support
827. scripts/grub.cfg: Add kernel boot parameter editing
828. scripts/grub.cfg: Add entry for test kernel
829. scripts/grub.cfg: Add rescue boot mode
830. docker/Dockerfile.ci: Add ccache volume mount point
831. docker/Dockerfile.ci: Add Docker layer timestamp labels
832. docker/Dockerfile.ci: Add CI user and workspace setup
833. docker/Dockerfile.ci: Add shell completion and extras
834. .gitignore: Add build/ directory entries
835. .gitignore: Add editor swap files and .DS_Store
836. .gitignore: Add compile_commands.json
837. .editorconfig: Create with kernel coding style
838. .clang-format: Add project-specific config (if missing)
839. .clang-tidy: Add project-specific checks config
840. .shellcheckrc: Add shellcheck config for scripts

---

## P09: Security Hardening (items 841-900)

Add security mitigations, hardening features, and improve existing protections.

841. KASLR: Add kernel address space layout randomization
842. KPTI: Enable user-space page table isolation on all CPUs
843. SMAP/SMEP: Enable Supervisor Mode Access Prevention
844. SMAP/SMEP: Add missing SMAP stac/clac pairs in copy_to/from_user
845. Stack canary: Add per-process canary value
846. Stack canary: Add canary check on context switch
847. Stack canary: Add canary verification in schedule_tail
848. NX: Verify NX bit is set on all non-executable pages
849. NX: Add NX bit enforcement in load_elf_binary
850. ASLR: Add stack randomization on exec
851. ASLR: Add mmap base randomization
852. ASLR: Add heap randomization via brk
853. ASLR: Add library load address randomization for ELF
854. W^X: Enforce write XOR execute on all user mappings
855. W^X: Add mprotect W^X enforcement
856. W^X: Add mmap MAP_SHARED W^X check
857. User access: Add __user annotation hardening with sparse
858. User access: Add probe_kernel_read for safe kernel memory access
859. User access: Add probe_kernel_write for safe kernel writes
860. User access: Add access_ok check before all copy_from_user
861. User access: Add access_ok check before all copy_to_user
862. User access: Add access_ok check before all strncpy_from_user
863. User access: Add hardened usercopy with slab object size check
864. User access: Add hardened usercopy with heap object size check
865. User access: Add hardened usercopy with stack object size check
866. User access: Add strncpy_from_user with NUL-termination guarantee
867. User access: Add clear_user helper
868. Credentials: Add security context in process credential check
869. Credentials: Add capability check before privilege escalation
870. Credentials: Add group ID validation in setgroups
871. Credentials: Add user namespace owner check
872. Audit: Add syscall audit logging for security events
873. Audit: Add file access audit events
874. Audit: Add process creation audit events
875. Seccomp: Add seccomp_filter with RET_KILL on rules violation
876. Seccomp: Add seccomp_get_action_avail for action discovery
877. Seccomp: Add seccomp(SCMP_ACT_LOG) for non-fatal logging
878. IMA: Add file integrity measurement on read
879. IMA: Add file integrity appraisal on execute
880. IMA: Add PCR extend for TPM attestation
881. EVM: Add extended verification of security xattrs
882. Landlock: Add file path access control
883. Landlock: Add network access control rules
884. Landlock: Add file system topology restriction
885. YAMA: Add ptrace scope restriction (0=default, 1=restricted)
886. YAMA: Add ptrace access check for non-related processes
887. Kernel lockdown: Add EFI secure boot integration
888. Kernel lockdown: Add module signature requirement when locked
889. Kernel lockdown: Add kexec disallowed when locked
890. Kernel lockdown: Add hibernation disallowed when locked
891. Kernel lockdown: Add /dev/mem and /dev/kmem restriction
892. Randomize struct member placement (randstruct) for key structures
893. Add CFI (Control Flow Integrity) for indirect function calls
894. Add SCS (Shadow Call Stack) for return address protection
895. Add CET (Control-flow Enforcement Technology) for IBT
896. Add kernel stack erasure on syscall return (stackleak)
897. Add kernel stack guard pages
898. Add WARN_ON_ONCE for unreachable security violations
899. Add BUG_ON data validation in security-critical paths
900. Add should-not-occur assertions in privilege checks

---

## P10: Performance (items 901-950)

Optimize kernel and userspace performance.

901. Memory: Add per-CPU page allocator caches (like Linux pcplist)
902. Memory: Add slab allocator per-CPU caches
903. Memory: Add huge page (2MB/1GB) support in VMM
904. Memory: Add THP (Transparent Huge Pages) for anonymous mappings
905. Memory: Add page fault fast-path (minor faults without MMU lock)
906. Memory: Add batched page freeing (free_batch)
907. Memory: Add page cache readahead with window tracking
908. Memory: Add mlock batching for bulk locking
909. Memory: Add zswap compressed swap cache
910. Memory: Add zram compressed RAM block device optimization
911. Memory: Add memory compaction for contiguous allocation
912. Memory: Add MGLRU (Multi-Gen LRU) page reclaim
913. Scheduler: Add per-CPU runqueues to reduce lock contention
914. Scheduler: Add wake-up preemption for interactive tasks
915. Scheduler: Add group scheduling (sched_group)
916. Scheduler: Add SCHED_BATCH for non-interactive tasks
917. Scheduler: Add SCHED_IDLE for low-priority tasks
918. Scheduler: Add load balancing between CPUs
919. Scheduler: Add tickless (nohz) mode for idle CPUs
920. Scheduler: Add RCU priority boosting
921. Process: Add fast-path for fork (CLONE_VM copy-on-write)
922. Process: Add vfork optimization (suspend parent until exec)
923. Process: Add CLONE_CHILD_CLEARTID for futex-based thread exit
924. Process: Add CLONE_CHILD_SETTID for thread ID in child memory
925. Process: Add CLONE_PARENT_SETTID for thread ID in parent memory
926. Process: Add CLONE_VFORK for vfork semantics
927. IPC: Add futex fast-path with atomic operations
928. IPC: Add futex PI (Priority Inheritance) for robust futex
929. IPC: Add eventfd batch read/write for bulk events
930. IPC: Add pipe splice for zero-cove pipe-to-pipe transfer
931. IPC: Add signalfd read optimization
932. IPC: Add mqueue non-blocking fast path
933. Networking: Add TCP segmentation offload in virtio-net
934. Networking: Add GRO (Generic Receive Offload) layer
935. Networking: Add GSO (Generic Segmentation Offload) layer
936. Networking: Add TCP output batching for aggregated sends
937. Networking: Add RPS (Receive Packet Steering) for CPU load spreading
938. Networking: Add netdev budget for polled NAPI-like receive
939. Networking: Add TCP pacing for burst reduction
940. Networking: Add conntrack hash table resize
941. Networking: Add socket buffer recycling (skb pool)
942. Networking: Add TCP corking for small-packet aggregation
943. Filesystem: Add buffer cache LRU2Q algorithm
944. Filesystem: Add page cache readahead window size optimization
945. Filesystem: Add fscache for network filesystem caching
946. Filesystem: Add inode cache slab for VFS
947. Filesystem: Add dentry cache (dcache) for path resolution
948. Filesystem: Add directory entry cache in ext2/fat32
949. Filesystem: Add data=ordered journal mode for ext4
950. Filesystem: Add delayed allocation for ext4 writeback

---

## P11: CI/CD & Automation (items 951-980)

Improve GitHub Actions workflows, add automation, fix CI gaps.

951. .github/workflows/ci.yml: Add `make check-whitespace` to verify job
952. .github/workflows/ci.yml: Add weekly full test run schedule (cron)
953. .github/workflows/ci.yml: Add disk image retention policy
954. .github/workflows/ci.yml: Add ccache stats to all build job summaries
955. .github/workflows/ci.yml: Add make version to all build jobs
956. .github/workflows/ci.yml: Add kernel binary size trend tracking
957. .github/workflows/ci.yml: Add per-job timeout annotation
958. .github/workflows/ci.yml: Add build artifact expiration metadata
959. .github/workflows/ci.yml: Add matrix build across GCC versions
960. .github/workflows/ci.yml: Add matrix build for clang too
961. .github/workflows/ci.yml: Add `make test` step in full-build job
962. .github/workflows/ci-pr.yml: Add in-repo label check before build
963. .github/workflows/ci-pr.yml: Add size diff annotation on PR
964. .github/workflows/ci-pr.yml: Add check for binary size regression
965. .github/workflows/ci-pr.yml: Add auto-label for subsystem changes
966. .github/workflows/ci-pr.yml: Add PR comment with build summary
967. .github/workflows/ci-pr.yml: Add `needs` chain optimization
968. .github/workflows/ci-pr.yml: Add matrix of GCC/clang per PR
969. .github/workflows/docker-ci-image.yml: Add scheduled rebuild
970. .github/workflows/docker-ci-image.yml: Add multi-arch build
971. .github/workflows/docker-ci-image.yml: Add image vulnerability scan
972. .github/workflows/ci.yml: Add e2e-smoke job to push CI
973. .github/workflows/ci.yml: Add host-side libc test job to CI
974. .github/workflows/ci.yml: Add `make check-padded` to CI
975. .github/workflows/ci.yml: Add `make check-free-nonheap` to CI
976. .github/workflows/ci.yml: Add dependency review action
977. .github/workflows/ci.yml: Add label-based conditional skips
978. .github/workflows/ci.yml: Add workflow_call trigger for reuse
979. .github/workflows/docker-ci-image.yml: Add latest tag pin
980. .github/workflows/ci.yml: Add CodeQL analysis on push

---

## P12: Final Polish (items 981-1000)

Crosscutting cleanup, meta-tasks, finishing touches.

981. Merge duplicate .gitignore entries (root vs subdir)
982. Add AUTHORS file with contributor credit
983. Add CHANGELOG.md with release notes format
984. Add CONTRIBUTING.md with PR and coding guidelines
985. Add LICENSE file (verify existing or add GPL/MIT)
986. Add CODE_OF_CONDUCT.md
987. Add SECURITY.md with vulnerability reporting policy
988. Add SUPPORT.md with issue and discussion guidance
989. Add ROADMAP.md with planned features
990. Add TODO.md canonical todo list
991. Review all .c files for copyright header consistency
992. Review all .h files for copyright header consistency
993. Add SPDX license identifiers to all source files
994. Standardize file header comments across all source
995. Run `make check-whitespace` — fix all trailing whitespace
996. Run `make lint` — fix all cppcheck/clang-tidy warnings
997. Run `make check-full` — fix all strict warning regressions
998. Run `make unit-test` — ensure all tests pass
999. Run full make + verify cycle — zero warnings end to end
1000. Tag release v1.0.0 and push with release notes
