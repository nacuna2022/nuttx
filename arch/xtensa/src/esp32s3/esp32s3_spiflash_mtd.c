/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_spiflash_mtd.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <assert.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/init.h>
#include <nuttx/mutex.h>
#include <nuttx/mtd/mtd.h>

#include "hardware/esp32s3_soc.h"
#include "hardware/esp32s3_cache_memory.h"

#include "xtensa_attr.h"
#include "esp32s3_spiflash.h"
#include "esp32s3_spiram.h"

#include "rom/esp32s3_spiflash.h"
#include "esp32s3_spiflash_mtd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MTD_BLK_SIZE                CONFIG_ESP32S3_SPIFLASH_MTD_BLKSIZE
#define MTD_ERASE_SIZE              4096
#define MTD_ERASED_STATE            (0xff)

#define MTD2PRIV(_dev)              ((struct esp32s3_mtd_dev_s *)_dev)
#define MTD_SIZE(_priv)             ((*(_priv)->data)->chip.chip_size)
#define MTD_BLK2SIZE(_priv, _b)     (MTD_BLK_SIZE * (_b))
#define MTD_SIZE2BLK(_priv, _s)     ((_s) / MTD_BLK_SIZE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
/* SPI flash work operation code */

enum spiflash_op_code_e
{
  SPIFLASH_OP_CODE_WRITE = 0,
  SPIFLASH_OP_CODE_READ,
  SPIFLASH_OP_CODE_ERASE,
  SPIFLASH_OP_CODE_SET_BANK,
  SPIFLASH_OP_CODE_ENCRYPT_READ,
  SPIFLASH_OP_CODE_ENCRYPT_WRITE
};
#endif

/* ESP32-S3 SPI Flash device private data  */

struct esp32s3_mtd_dev_s
{
  struct mtd_dev_s mtd;

  /* SPI Flash data */

  const struct spiflash_legacy_data_s **data;
};

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
/* SPI flash work operation arguments */

struct spiflash_work_arg
{
  enum spiflash_op_code_e op_code;

  struct
  {
    uint32_t addr;
    uint8_t *buffer;
    uint32_t size;
    uint32_t paddr;
  } op_arg;

  volatile int ret;

  sem_t sem;
};
#endif

/****************************************************************************
 * Private Functions Prototypes
 ****************************************************************************/

/* MTD driver methods */

static int esp32s3_erase(struct mtd_dev_s *dev, off_t startblock,
                         size_t nblocks);
static ssize_t esp32s3_read(struct mtd_dev_s *dev, off_t offset,
                            size_t nbytes, uint8_t *buffer);
static ssize_t esp32s3_read_decrypt(struct mtd_dev_s *dev,
                                    off_t offset,
                                    size_t nbytes,
                                    uint8_t *buffer);
static ssize_t esp32s3_bread(struct mtd_dev_s *dev, off_t startblock,
                             size_t nblocks, uint8_t *buffer);
static ssize_t esp32s3_bread_decrypt(struct mtd_dev_s *dev,
                                     off_t startblock,
                                     size_t nblocks,
                                     uint8_t *buffer);
static ssize_t esp32s3_write(struct mtd_dev_s *dev, off_t offset,
                             size_t nbytes, const uint8_t *buffer);
static ssize_t esp32s3_bwrite(struct mtd_dev_s *dev, off_t startblock,
                              size_t nblocks, const uint8_t *buffer);
static ssize_t esp32s3_bwrite_encrypt(struct mtd_dev_s *dev,
                                      off_t startblock,
                                      size_t nblocks,
                                      const uint8_t *buffer);
static int esp32s3_ioctl(struct mtd_dev_s *dev, int cmd,
                         unsigned long arg);

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
static inline bool IRAM_ATTR stack_is_psram(void);
static void esp32s3_spiflash_work(void *arg);
static int esp32s3_async_op(enum spiflash_op_code_e opcode,
                            uint32_t addr,
                            const uint8_t *buffer,
                            uint32_t size,
                            uint32_t paddr);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct esp32s3_mtd_dev_s g_esp32s3_spiflash =
{
  .mtd =
          {
            .erase  = esp32s3_erase,
            .bread  = esp32s3_bread,
            .bwrite = esp32s3_bwrite,
            .read   = esp32s3_read,
            .ioctl  = esp32s3_ioctl,
#ifdef CONFIG_MTD_BYTE_WRITE
            .write  = esp32s3_write,
#endif
            .name   = "esp32s3_spiflash"
          },
  .data = (const struct spiflash_legacy_data_s **)
          (&rom_spiflash_legacy_data),
};

static const struct esp32s3_mtd_dev_s g_esp32s3_spiflash_encrypt =
{
  .mtd =
          {
            .erase  = esp32s3_erase,
            .bread  = esp32s3_bread_decrypt,
            .bwrite = esp32s3_bwrite_encrypt,
            .read   = esp32s3_read_decrypt,
            .ioctl  = esp32s3_ioctl,
#ifdef CONFIG_MTD_BYTE_WRITE
            .write  = NULL,
#endif
            .name   = "esp32s3_spiflash_encrypt"
          },
  .data = (const struct spiflash_legacy_data_s **)
          (&rom_spiflash_legacy_data),
};

/* Ensure exclusive access to the driver */

static mutex_t g_lock = NXMUTEX_INITIALIZER;

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
static struct work_s g_work;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stack_is_psram
 *
 * Description:
 *   Check if current task's stack space is in PSRAM.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   true if it is in PSRAM or false if not.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
static inline bool IRAM_ATTR stack_is_psram(void)
{
  void *sp = (void *)up_getsp();

  return esp32s3_ptr_extram(sp);
}
#endif

/****************************************************************************
 * Name: esp32s3_spiflash_work
 *
 * Description:
 *   Do SPI Flash operation, cache result and send semaphore to wake up
 *   blocked task.
 *
 * Input Parameters:
 *   arg - Reference to SPI flash work arguments structure (cast to void*)
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
static void esp32s3_spiflash_work(void *arg)
{
  struct spiflash_work_arg *work_arg = (struct spiflash_work_arg *)arg;

  if (work_arg->op_code == SPIFLASH_OP_CODE_WRITE)
    {
      work_arg->ret = spi_flash_write(work_arg->op_arg.addr,
                                      work_arg->op_arg.buffer,
                                      work_arg->op_arg.size);
    }
  else if (work_arg->op_code == SPIFLASH_OP_CODE_READ)
    {
      work_arg->ret = spi_flash_read(work_arg->op_arg.addr,
                                     work_arg->op_arg.buffer,
                                     work_arg->op_arg.size);
    }
  else if (work_arg->op_code == SPIFLASH_OP_CODE_ERASE)
    {
      work_arg->ret = spi_flash_erase_range(work_arg->op_arg.addr,
                                            work_arg->op_arg.size);
    }
  else if (work_arg->op_code == SPIFLASH_OP_CODE_SET_BANK)
    {
      work_arg->ret = cache_dbus_mmu_map(work_arg->op_arg.addr,
                                         work_arg->op_arg.paddr,
                                         work_arg->op_arg.size);
    }
  else if (work_arg->op_code == SPIFLASH_OP_CODE_ENCRYPT_READ)
    {
      work_arg->ret = spi_flash_read_encrypted(work_arg->op_arg.addr,
                                               work_arg->op_arg.buffer,
                                               work_arg->op_arg.size);
    }
  else if (work_arg->op_code == SPIFLASH_OP_CODE_ENCRYPT_WRITE)
    {
      work_arg->ret = spi_flash_write_encrypted(work_arg->op_arg.addr,
                                                work_arg->op_arg.buffer,
                                                work_arg->op_arg.size);
    }
  else
    {
      ferr("ERROR: op_code=%d is not supported\n", work_arg->op_code);
    }

  nxsem_post(&work_arg->sem);
}

/****************************************************************************
 * Name: esp32s3_async_op
 *
 * Description:
 *   Send operation code and arguments to workqueue so that workqueue do SPI
 *   Flash operation actually.
 *
 * Input Parameters:
 *   opcode - SPI flash work operation code
 *   addr   - target address
 *   buffer - data buffer pointer
 *   size   - data number
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int esp32s3_async_op(enum spiflash_op_code_e opcode,
                            uint32_t addr,
                            const uint8_t *buffer,
                            uint32_t size,
                            uint32_t paddr)
{
  int ret;
  struct spiflash_work_arg work_arg =
  {
    .op_code = opcode,
    .op_arg =
    {
      .addr = addr,
      .buffer = (uint8_t *)buffer,
      .size = size,
      .paddr = paddr,
    },
    .sem = NXSEM_INITIALIZER(0, 0)
  };

  ret = work_queue(LPWORK, &g_work, esp32s3_spiflash_work, &work_arg, 0);
  if (ret == 0)
    {
      nxsem_wait(&work_arg.sem);
      ret = work_arg.ret;
    }

  return ret;
}
#endif

/****************************************************************************
 * Name: esp32s3_erase
 *
 * Description:
 *   Erase SPI Flash designated sectors.
 *
 * Input Parameters:
 *   dev        - MTD device data
 *   startblock - start block number, it is not equal to SPI Flash's block
 *   nblocks    - Number of blocks
 *
 * Returned Value:
 *   Erased blocks if success or a negative value if fail.
 *
 ****************************************************************************/

static int esp32s3_erase(struct mtd_dev_s *dev, off_t startblock,
                         size_t nblocks)
{
  ssize_t ret;
  uint32_t offset = startblock * MTD_ERASE_SIZE;
  uint32_t nbytes = nblocks * MTD_ERASE_SIZE;
  struct esp32s3_mtd_dev_s *priv = (struct esp32s3_mtd_dev_s *)dev;

  if ((offset > MTD_SIZE(priv)) || ((offset + nbytes) > MTD_SIZE(priv)))
    {
      return -EINVAL;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d)\n", __func__, dev, startblock, nblocks);

  finfo("spi_flash_erase_range(0x%x, %d)\n", offset, nbytes);
#endif

  ret = nxmutex_lock(&g_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
  if (stack_is_psram())
    {
      ret = esp32s3_async_op(SPIFLASH_OP_CODE_ERASE, offset, NULL,
                             nbytes, 0);
    }
  else
#endif
    {
      ret = spi_flash_erase_range(offset, nbytes);
    }

  nxmutex_unlock(&g_lock);
  if (ret == OK)
    {
      ret = nblocks;
    }
  else
    {
#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
      finfo("Failed to erase the flash range!\n");
#endif
      ret = -1;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif

  return ret;
}

/****************************************************************************
 * Name: esp32s3_read
 *
 * Description:
 *   Read data from SPI Flash at designated address.
 *
 * Input Parameters:
 *   dev    - MTD device data
 *   offset - target address offset
 *   nbytes - data number
 *   buffer - data buffer pointer
 *
 * Returned Value:
 *   Read data bytes if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_read(struct mtd_dev_s *dev, off_t offset,
                          size_t nbytes, uint8_t *buffer)
{
  ssize_t ret;

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, offset, nbytes, buffer);

  finfo("spi_flash_read(0x%x, %p, %d)\n", offset, buffer, nbytes);
#endif

  /* Acquire the mutex. */

  ret = nxmutex_lock(&g_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
  if (stack_is_psram())
    {
      ret = esp32s3_async_op(SPIFLASH_OP_CODE_READ, offset,
                             buffer, nbytes, 0);
    }
  else
#endif
    {
      ret = spi_flash_read(offset, buffer, nbytes);
    }

  nxmutex_unlock(&g_lock);
  if (ret == OK)
    {
      ret = nbytes;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif

  return ret;
}

/****************************************************************************
 * Name: esp32s3_bread
 *
 * Description:
 *   Read data from designated blocks.
 *
 * Input Parameters:
 *   dev        - MTD device data
 *   startblock - start block number, it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *   buffer     - data buffer pointer
 *
 * Returned Value:
 *   Read block number if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_bread(struct mtd_dev_s *dev, off_t startblock,
                           size_t nblocks, uint8_t *buffer)
{
  ssize_t ret;
  uint32_t addr = startblock * MTD_BLK_SIZE;
  uint32_t size = nblocks * MTD_BLK_SIZE;

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, startblock, nblocks,
        buffer);
#endif

  ret = esp32s3_read(dev, addr, size, buffer);
  if (ret == size)
    {
      ret = nblocks;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif

  return ret;
}

/****************************************************************************
 * Name: esp32s3_read_decrypt
 *
 * Description:
 *   Read encrypted data and decrypt automatically from SPI Flash
 *   at designated address.
 *
 * Input Parameters:
 *   dev    - MTD device data
 *   offset - target address offset
 *   nbytes - data number
 *   buffer - data buffer pointer
 *
 * Returned Value:
 *   Read data bytes if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_read_decrypt(struct mtd_dev_s *dev,
                                  off_t offset,
                                  size_t nbytes,
                                  uint8_t *buffer)
{
  ssize_t ret;

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, offset, nbytes, buffer);

  finfo("spi_flash_read_encrypted(0x%x, %p, %d)\n", offset, buffer, nbytes);
#endif

  /* Acquire the mutex. */

  ret = nxmutex_lock(&g_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
  if (stack_is_psram())
    {
      ret = esp32s3_async_op(SPIFLASH_OP_CODE_ENCRYPT_READ, offset,
                             buffer, nbytes, 0);
    }
  else
#endif
    {
      ret = spi_flash_read_encrypted(offset, buffer, nbytes);
    }

  nxmutex_unlock(&g_lock);
  if (ret == OK)
    {
      ret = nbytes;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif

  return ret;
}

/****************************************************************************
 * Name: esp32s3_bread_decrypt
 *
 * Description:
 *   Read encrypted data and decrypt automatically from designated blocks.
 *
 * Input Parameters:
 *   dev        - MTD device data
 *   startblock - start block number, it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *   buffer     - data buffer pointer
 *
 * Returned Value:
 *   Read block number if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_bread_decrypt(struct mtd_dev_s *dev,
                                     off_t startblock,
                                     size_t nblocks,
                                     uint8_t *buffer)
{
  ssize_t ret;
  uint32_t addr = startblock * MTD_BLK_SIZE;
  uint32_t size = nblocks * MTD_BLK_SIZE;

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, startblock, nblocks,
        buffer);
#endif

  ret = esp32s3_read_decrypt(dev, addr, size, buffer);
  if (ret == size)
    {
      ret = nblocks;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif
  return ret;
}

/****************************************************************************
 * Name: esp32s3_write
 *
 * Description:
 *   write data to SPI Flash at designated address.
 *
 * Input Parameters:
 *   dev    - MTD device data
 *   offset - target address offset
 *   nbytes - data number
 *   buffer - data buffer pointer
 *
 * Returned Value:
 *   Writen bytes if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_write(struct mtd_dev_s *dev, off_t offset,
                             size_t nbytes, const uint8_t *buffer)
{
  ssize_t ret;
  struct esp32s3_mtd_dev_s *priv = (struct esp32s3_mtd_dev_s *)dev;

  ASSERT(buffer);

  if ((offset > MTD_SIZE(priv)) || ((offset + nbytes) > MTD_SIZE(priv)))
    {
      return -EINVAL;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, offset, nbytes, buffer);

  finfo("spi_flash_write(0x%x, %p, %d)\n", offset, buffer, nbytes);
#endif

  /* Acquire the mutex. */

  ret = nxmutex_lock(&g_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
  if (stack_is_psram())
    {
      ret = esp32s3_async_op(SPIFLASH_OP_CODE_WRITE, offset,
                             buffer, nbytes, 0);
    }
  else
#endif
    {
      ret = spi_flash_write(offset, buffer, nbytes);
    }

  nxmutex_unlock(&g_lock);
  if (ret == OK)
    {
      ret = nbytes;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif
  return ret;
}

/****************************************************************************
 * Name: esp32s3_bwrite_encrypt
 *
 * Description:
 *   Write data to designated blocks by SPI Flash hardware encryption.
 *
 * Input Parameters:
 *   dev        - MTD device data
 *   startblock - start MTD block number,
 *                it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *   buffer     - data buffer pointer
 *
 * Returned Value:
 *   Writen block number if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_bwrite_encrypt(struct mtd_dev_s *dev,
                                      off_t startblock,
                                      size_t nblocks,
                                      const uint8_t *buffer)
{
  ssize_t ret;
  uint32_t addr = startblock * MTD_BLK_SIZE;
  uint32_t size = nblocks * MTD_BLK_SIZE;

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, startblock,
        nblocks, buffer);

  finfo("spi_flash_write_encrypted(0x%x, %p, %d)\n", addr, buffer, size);
#endif

  ret = nxmutex_lock(&g_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
  if (stack_is_psram())
    {
      ret = esp32s3_async_op(SPIFLASH_OP_CODE_ENCRYPT_WRITE, addr,
                             buffer, size, 0);
    }
  else
#endif
    {
      ret = spi_flash_write_encrypted(addr, buffer, size);
    }

  nxmutex_unlock(&g_lock);
  if (ret == OK)
    {
      ret = nblocks;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif
  return ret;
}

/****************************************************************************
 * Name: esp32s3_bwrite
 *
 * Description:
 *   Write data to designated blocks.
 *
 * Input Parameters:
 *   dev        - MTD device data
 *   startblock - start MTD block number,
 *                it is not equal to SPI Flash's block
 *   nblocks    - blocks number
 *   buffer     - data buffer pointer
 *
 * Returned Value:
 *   Writen block number if success or a negative value if fail.
 *
 ****************************************************************************/

static ssize_t esp32s3_bwrite(struct mtd_dev_s *dev, off_t startblock,
                              size_t nblocks, const uint8_t *buffer)
{
  ssize_t ret;
  uint32_t addr = startblock * MTD_BLK_SIZE;
  uint32_t size = nblocks * MTD_BLK_SIZE;

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s(%p, 0x%x, %d, %p)\n", __func__, dev, startblock,
        nblocks, buffer);
#endif

  ret = esp32s3_write(dev, addr, size, buffer);
  if (ret == size)
    {
      ret = nblocks;
    }

#ifdef CONFIG_ESP32S3_STORAGE_MTD_DEBUG
  finfo("%s()=%d\n", __func__, ret);
#endif

  return ret;
}

/****************************************************************************
 * Name: esp32s3_ioctl
 *
 * Description:
 *   Set/Get option to/from ESP32-S3 SPI Flash MTD device data.
 *
 * Input Parameters:
 *   dev - ESP32-S3 MTD device data
 *   cmd - operation command
 *   arg - operation argument
 *
 * Returned Value:
 *   0 if success or a negative value if fail.
 *
 ****************************************************************************/

static int esp32s3_ioctl(struct mtd_dev_s *dev, int cmd,
                         unsigned long arg)
{
  int ret = OK;
  finfo("cmd: %d\n", cmd);

  switch (cmd)
    {
      case MTDIOC_GEOMETRY:
        {
          struct esp32s3_mtd_dev_s *priv = (struct esp32s3_mtd_dev_s *)dev;
          struct mtd_geometry_s *geo = (struct mtd_geometry_s *)arg;
          if (geo)
            {
              memset(geo, 0, sizeof(*geo));

              geo->blocksize    = MTD_BLK_SIZE;
              geo->erasesize    = MTD_ERASE_SIZE;
              geo->neraseblocks = MTD_SIZE(priv) / MTD_ERASE_SIZE;
              ret               = OK;

              finfo("blocksize: %" PRId32 " erasesize: %" PRId32 \
                    " neraseblocks: %" PRId32 "\n",
                    geo->blocksize, geo->erasesize, geo->neraseblocks);
            }
        }
        break;

      case BIOC_PARTINFO:
        {
          struct esp32s3_mtd_dev_s *priv = (struct esp32s3_mtd_dev_s *)dev;
          struct partition_info_s *info = (struct partition_info_s *)arg;
          if (info != NULL)
            {
              info->numsectors  = MTD_SIZE(priv) / MTD_BLK_SIZE;
              info->sectorsize  = MTD_BLK_SIZE;
              info->startsector = 0;
              info->parent[0]   = '\0';
            }
        }
        break;

      case MTDIOC_ERASESTATE:
        {
          uint8_t *result = (uint8_t *)arg;
          *result = MTD_ERASED_STATE;

          ret = OK;
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  finfo("return %d\n", ret);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_set_bank
 *
 * Description:
 *   Set Ext-SRAM-Cache mmu mapping.
 *
 * Input Parameters:
 *   virt_bank - Beginning of the virtual bank
 *   phys_bank - Beginning of the physical bank
 *   ct        - Number of banks
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void esp32s3_set_bank(int virt_bank, int phys_bank, int ct)
{
  int ret;
  uint32_t vaddr = SOC_EXTRAM_DATA_LOW + MMU_PAGE_SIZE * virt_bank;
  uint32_t paddr = phys_bank * MMU_PAGE_SIZE;
#ifdef CONFIG_ESP32S3_SPI_FLASH_SUPPORT_PSRAM_STACK
  if (stack_is_psram())
    {
      ret = esp32s3_async_op(SPIFLASH_OP_CODE_SET_BANK, vaddr, NULL, ct,
                             paddr);
    }
  else
#endif
    {
      ret = cache_dbus_mmu_map(vaddr, paddr, ct);
    }

  DEBUGASSERT(ret == 0);
  UNUSED(ret);
}

/****************************************************************************
 * Name: esp32s3_spiflash_alloc_mtdpart
 *
 * Description:
 *   Allocate an MTD partition from the ESP32-S3 SPI Flash.
 *
 * Input Parameters:
 *   mtd_offset - MTD Partition offset from the base address in SPI Flash.
 *   mtd_size   - Size for the MTD partition.
 *   encrypted  - Flag indicating whether the newly allocated partition will
 *                have its content encrypted.
 *
 * Returned Value:
 *   ESP32-S3 SPI Flash MTD data pointer if success or NULL if fail.
 *
 ****************************************************************************/

struct mtd_dev_s *esp32s3_spiflash_alloc_mtdpart(uint32_t mtd_offset,
                                                 uint32_t mtd_size,
                                                 bool encrypted)
{
  const struct esp32s3_mtd_dev_s *priv;
  const esp32s3_spiflash_chip_t *chip;
  struct mtd_dev_s *mtd_part;
  uint32_t blocks;
  uint32_t startblock;
  uint32_t size;

  if (encrypted)
    {
      priv = &g_esp32s3_spiflash_encrypt;
    }
  else
    {
      priv = &g_esp32s3_spiflash;
    }

  chip = &(*priv->data)->chip;

  finfo("ESP32-S3 SPI Flash information:\n");
  finfo("\tID = 0x%" PRIx32 "\n", chip->device_id);
  finfo("\tStatus mask = 0x%" PRIx32 "\n", chip->status_mask);
  finfo("\tChip size = %" PRId32 " KB\n", chip->chip_size / 1024);
  finfo("\tPage size = %" PRId32 " B\n", chip->page_size);
  finfo("\tSector size = %" PRId32 " KB\n", chip->sector_size / 1024);
  finfo("\tBlock size = %" PRId32 " KB\n", chip->block_size / 1024);

  ASSERT((mtd_offset + mtd_size) <= chip->chip_size);
  ASSERT((mtd_offset % chip->sector_size) == 0);
  ASSERT((mtd_size % chip->sector_size) == 0);

  if (mtd_size == 0)
    {
      size = chip->chip_size - mtd_offset;
    }
  else
    {
      size = mtd_size;
    }

  finfo("\tMTD offset = 0x%" PRIx32 "\n", mtd_offset);
  finfo("\tMTD size = 0x%" PRIx32 "\n", size);

  startblock = MTD_SIZE2BLK(priv, mtd_offset);
  blocks = MTD_SIZE2BLK(priv, size);

  mtd_part = mtd_partition((struct mtd_dev_s *)&priv->mtd, startblock,
                           blocks);
  if (!mtd_part)
    {
      ferr("ERROR: Failed to create MTD partition\n");
      return NULL;
    }

  return mtd_part;
}

/****************************************************************************
 * Name: esp32s3_spiflash_mtd
 *
 * Description:
 *   Get SPI Flash MTD.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   ESP32-S3 SPI Flash MTD pointer.
 *
 ****************************************************************************/

struct mtd_dev_s *esp32s3_spiflash_mtd(void)
{
  struct esp32s3_mtd_dev_s *priv =
      (struct esp32s3_mtd_dev_s *)&g_esp32s3_spiflash;

  return &priv->mtd;
}

/****************************************************************************
 * Name: esp32s3_spiflash_encrypt_mtd
 *
 * Description:
 *   Get SPI Flash encryption MTD.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   SPI Flash encryption MTD pointer.
 *
 ****************************************************************************/

struct mtd_dev_s *esp32s3_spiflash_encrypt_mtd(void)
{
  struct esp32s3_mtd_dev_s *priv =
      (struct esp32s3_mtd_dev_s *)&g_esp32s3_spiflash_encrypt;

  return &priv->mtd;
}
