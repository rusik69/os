#ifndef DMA_ENGINE_H
#define DMA_ENGINE_H

#include "types.h"

/* DMA channel directions */
#define DMA_READ   0  /* Memory -> I/O */
#define DMA_WRITE  1  /* I/O -> Memory */

/* DMA transfer modes */
#define DMA_MODE_ON_DEMAND  0
#define DMA_MODE_SINGLE     1
#define DMA_MODE_BLOCK      2
#define DMA_MODE_CASCADE    3

/* DMA page/address registers for 8237 */
#define DMA_CHAN_ADDR(ch)  ((ch) < 4 ? (0x00 + (ch) * 2) : (0xC0 + ((ch) - 4) * 2))
#define DMA_CHAN_COUNT(ch) ((ch) < 4 ? (0x01 + (ch) * 2) : (0xC2 + ((ch) - 4) * 2))
#define DMA_CHAN_PAGE(ch)  ((ch) < 4 ? (0x87 + (ch)) : (0x8B + (ch)))

#define DMA_CMD_REG    0x08   /* Command register (master) */
#define DMA_CMD_REG2   0xD0   /* Command register (slave) */
#define DMA_MASK_REG   0x0A   /* Single mask register */
#define DMA_MASK_REG2  0xD4
#define DMA_MODE_REG   0x0B   /* Mode register */
#define DMA_MODE_REG2  0xD6
#define DMA_CLR_FF     0x0C   /* Clear flip-flop */
#define DMA_CLR_FF2    0xD8
#define DMA_MASK_ALL   0x0F   /* Mask all bits */
#define DMA_MASK_ALL2  0xDE
#define DMA_STATUS     0x08   /* Status register */
#define DMA_STATUS2    0xD0

/* DMA engine status */
#define DMA_OK      0
#define DMA_ERR     -1

/* DMA channel descriptor */
struct dma_chan {
    int      used;
    int      channel;      /* 0..7 */
    int      direction;
    int      mode;
    uint32_t addr;         /* Physical address (24-bit for channels 0-3, 32-bit for 4-7) */
    uint32_t count;        /* Transfer count in bytes */
};

/* API */
int  dma_init(void);
int  dma_request_channel(int channel, int direction, int mode);
int  dma_program(struct dma_chan *chan, uint32_t addr, uint32_t count);
void dma_start(int channel);
void dma_stop(int channel);
void dma_release_channel(int channel);
int  dma_is_initialized(void);

/* ISA DMA controller base (8237) */
extern volatile int dma_initialized;

#endif /* DMA_ENGINE_H */
