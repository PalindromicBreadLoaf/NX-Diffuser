/* G-Diffuser 64DD (Leo) drive replacement for the Expansion Kit.
 *
 * The decomp's leo/lib is real drive firmware traffic (command queues, motor,
 * interrupts); on PC the whole drive collapses to reads from a linear disk
 * image. LBA<->byte math comes from the decomp's own pure files
 * (leo/lib/bytetolba.c, lbatobyte.c, leo_tbl.c) compiled alongside this file;
 * everything hardware-shaped is implemented here as an immediate memcpy plus
 * a completion message, mirroring the port's other device managers.
 *
 * Compiled inside the gdiffuser_game target: decomp headers only, no MSVC CRT
 * includes (they clash with the decomp's libc headers). File I/O lives in
 * port/disk_buffer.cpp on the host side.
 */
#include "global.h"
#include "PR/leo.h"
#include "leo/leo_internal.h"

/* From PR/leoappli.h, which cannot be included alongside PR/leo.h (both
   define the LEOCmd packet types). */
#define LEO_STATUS_GOOD 0x00
#define LEO_STATUS_CHECK_CONDITION 0x02

/* Host side (port/disk_buffer.cpp) */
extern unsigned char* gdx_disk_buffer;
extern unsigned int gdx_disk_size;
extern int gdx_disk_load(void);

/* leo/lib internals: the pure translation files (leotranslat.c, leoutil.c,
   leo_tbl.c) index their zone tables by disk type. Read from the loaded
   image's system area by gdx_leo_on_disk_loaded(). */
u8 LEOdisk_type = 0;
bool __leoActive = 0;
/* Hardware-layer globals the translation math reads; the drive manager that
   would populate them is replaced by this file. */
leo_sys_form LEO_sys_data;
tgt_param_form LEOtgt_param;
LEOCmd* LEOcur_command = NULL;
s32 LEO_country_code = 0;

void gdx_leo_on_disk_loaded(const unsigned char* disk) {
    /* 64DD system area, byte 0x05: disk type (0-6) in the disk ID. */
    LEOdisk_type = disk[5] & 0xF;
    if (LEOdisk_type > 6) {
        LEOdisk_type = 0;
    }
    __leoActive = 1;
}

/* Completion for async-shaped commands: work is done synchronously, so post
 * completion immediately, matching what the drive manager thread would
 * eventually do. The message VALUE is the LEOError — consumers do
 * `osRecvMesg(mq, &sSLLeoError, ...)` and switch on it (sys_leo_dd.c), so
 * success must be posted as 0/NULL (LEO_ERROR_GOOD), never a pointer. */
static void LeoPostDone(LEOCmd* cmdBlock, OSMesgQueue* mq) {
    if (cmdBlock != NULL) {
        cmdBlock->header.status = LEO_STATUS_GOOD;
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)(uintptr_t)LEO_ERROR_GOOD, OS_MESG_NOBLOCK);
    }
}

u32 LeoDriveExist(void) {
    /* The EK boot probes the drive BEFORE creating any leo manager
       (sys_main.c: gLeoDriveConnectionState = LeoDriveExist()), so this is
       the first disk touch — attempt the (idempotent) image load here.
       "A drive with a disk exists" == "a disk image is available". */
    return gdx_disk_load() ? 1u : 0u;
}

s32 LeoCreateLeoManager(OSPri comPri, OSPri intPri, OSMesgQueue* mq, u32 cmdBufSize) {
    (void)comPri; (void)intPri; (void)mq; (void)cmdBufSize;
    return gdx_disk_load() ? LEO_ERROR_GOOD : LEO_ERROR_DRIVE_NOT_READY;
}

s32 LeoCJCreateLeoManager(OSPri comPri, OSPri intPri, OSMesgQueue* mq, u32 cmdBufSize) {
    return LeoCreateLeoManager(comPri, intPri, mq, cmdBufSize);
}

s32 LeoCACreateLeoManager(OSPri comPri, OSPri intPri, OSMesgQueue* mq, u32 cmdBufSize) {
    return LeoCreateLeoManager(comPri, intPri, mq, cmdBufSize);
}

s32 LeoClearQueue(void) {
    return LEO_ERROR_GOOD;
}

s32 LeoTestUnitReady(LEOStatus* status) {
    if (status != NULL) {
        *status = (gdx_disk_buffer != NULL) ? LEO_STATUS_GOOD : LEO_STATUS_CHECK_CONDITION;
    }
    return (gdx_disk_buffer != NULL) ? LEO_ERROR_GOOD : LEO_ERROR_DRIVE_NOT_READY;
}

s32 LeoReadWrite(LEOCmd* cmdBlock, s32 direction, u32 LBA, void* buffer, u32 nLBAs, OSMesgQueue* mq) {
    s32 offset = 0;
    s32 bytes = 0;

    if (gdx_disk_buffer == NULL) {
        return LEO_ERROR_DRIVE_NOT_READY;
    }
    if (LeoLBAToByte(0, LBA, &offset) != LEO_ERROR_GOOD ||
        LeoLBAToByte((s32)LBA, nLBAs, &bytes) != LEO_ERROR_GOOD) {
        return LEO_ERROR_LBA_OUT_OF_RANGE;
    }
    if ((u32)offset + (u32)bytes > gdx_disk_size) {
        return LEO_ERROR_LBA_OUT_OF_RANGE;
    }

    if (direction == OS_READ) {
        bcopy(gdx_disk_buffer + offset, buffer, bytes);
    } else {
        /* Writes (saves to disk) update the in-memory image only; persisting
           edited courses to host storage arrives with the save-support slice. */
        bcopy(buffer, gdx_disk_buffer + offset, bytes);
    }

    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoSeek(LEOCmd* cmdBlock, u32 lba, OSMesgQueue* mq) {
    (void)lba;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoRezero(LEOCmd* cmdBlock, OSMesgQueue* mq) {
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoSpdlMotor(LEOCmd* cmdBlock, LEOSpdlMode mode, OSMesgQueue* mq) {
    (void)mode;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoReadDiskID(LEOCmd* cmdBlock, LEODiskID* vaddr, OSMesgQueue* mq) {
    /* The game only sanity-checks gameName/gameVersion, which the EK code
       tolerates as zeros when the manager reports the drive present. */
    if (vaddr != NULL) {
        bzero(vaddr, sizeof(*vaddr));
    }
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoModeSelectAsync(LEOCmd* cmdBlock, u32 standby, u32 sleep, OSMesgQueue* mq) {
    (void)standby; (void)sleep;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoReadRTC(LEOCmd* cmdBlock, OSMesgQueue* mq) {
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoSetRTC(LEOCmd* cmdBlock, LEODiskTime* RTCdata, OSMesgQueue* mq) {
    (void)RTCdata;
    LeoPostDone(cmdBlock, mq);
    return LEO_ERROR_GOOD;
}

s32 LeoInquiry(LEOVersion* ver) {
    if (ver != NULL) {
        bzero(ver, sizeof(*ver));
    }
    return LEO_ERROR_GOOD;
}

s32 LeoReadCapacity(LEOCapacity* cmdBlock, s32 dir) {
    (void)dir;
    if (cmdBlock != NULL) {
        bzero(cmdBlock, sizeof(*cmdBlock));
    }
    return LEO_ERROR_GOOD;
}

void LeoBootGame(void* entry) {
    /* On hardware this jumps into the disk's boot program. In the source
       port every disk-side function is compiled in, so booting reduces to
       making sure the disk image (and its asset fills) are loaded. */
    (void)entry;
    gdx_disk_load();
}

/* MFS version B work buffer — a fixed expansion-RAM address on hardware
   (see include/leo/mfs.h); real storage here. */
u8 D_807801E0[0x4D10];
