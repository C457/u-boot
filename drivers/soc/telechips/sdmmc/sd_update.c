/****************************************************************************
 *   FileName    : fwdn_SDMMC.c
 *   Description : SDMMC F/W downloader function
 ****************************************************************************
 *
 *   TCC Version 1.0
 *   Copyright (c) Telechips, Inc.
 *   ALL RIGHTS RESERVED
 *
 ****************************************************************************/
#include <common.h>
#include <errno.h>

#include <asm/arch/fwdn/fwdn_drv_v7.h>
#include <asm/arch/fwdn/fwupgrade.h>

#include <asm/arch/fwdn/Disk.h>
#include <asm/arch/sdmmc/sd_memory.h>
#include <asm/arch/sdmmc/sd_update.h>


#define	LPRINTF	printf
#define	word_of(X)					( *(volatile unsigned int *)((X)) )

#define SUCCESS 0

//=============================================================================
//*
//*
//*                     [ EXTERN VARIABLE & FUNCTIONS DEFINE ]
//*
//*
//=============================================================================
extern const unsigned int		CRC32_TABLE[256];
extern unsigned int				g_uiFWDN_OverWriteSNFlag;
extern unsigned int				g_uiFWDN_WriteSNFlag;

extern unsigned int				FWDN_FNT_SetSN(unsigned char* ucTempData, unsigned int uiSNOffset);
extern void						FWDN_FNT_InsertSN(unsigned char * pSerialNumber);
extern void						FWDN_FNT_VerifySN(unsigned char* ucTempData, unsigned int uiSNOffset);
extern void						IO_DBG_SerialPrintf_(char *format, ...);
extern int	check_fwdn_mode(void);


//int FwdnUpdateGetBootSDSerial(char* SerialNum);

#define			CRC32_SIZE	4
unsigned int	global_buf[ FWDN_WRITE_BUFFER_SIZE ];

void mmc_init_page(
		page_t *p,
		unsigned long start_addr,
		unsigned long size,
		unsigned long hidden,
		unsigned char *buf,
		unsigned int boot_partition)
{
	p->start_page = start_addr;
	p->rw_size = size;
	p->hidden_index = hidden;
	p->buff = buf;
	p->boot_partition = boot_partition;
}

static int __do_mmc_update( page_t *p )
{
	page_t q;
	unsigned char *buf;

	if(p->rw_size == 0)
		return 0;

	if( DISK_Ioctl( DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_WRITE_PAGE, (void*)p ) )
		return -1;

	buf = p->buff + ((p->rw_size) << 9);
	mmc_init_page( &q, p->start_page, p->rw_size, p->hidden_index, buf, p->boot_partition );

	if( DISK_Ioctl( DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_READ_PAGE, (void*)&q ) ||
			memcmp( p->buff, q.buff, (p->rw_size) << 9 ) )
		return -1;
	return 0;
}


int mmc_update_valid_test(unsigned int rom_size)
{
	ioctl_diskinfo_t disk_info;

	if( DISK_Ioctl( DISK_DEVICE_TRIFLASH, DEV_GET_DISKINFO, (void *)&disk_info ) ||
			disk_info.sector_size != 512 ||
			!rom_size )
		return -1;

	return 0;
}

int mmc_get_serial_num(unsigned char* serial_num, unsigned int boot_partition)
{
	page_t p;
	BOOTSD_Header_Info_T buf;

	if( mmc_update_valid_test(1) )
		return -1;

	mmc_init_page( &p, BOOTSD_GetHeaderAddr( boot_partition ), 1, 0, (unsigned char *)&buf, boot_partition );

	if( DISK_Ioctl(DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_READ_PAGE, (void *)&p) )
		return -1;

	if( serial_num != NULL )
		memcpy(serial_num, buf.ucSerialNum, 64);

	FWDN_FNT_VerifySN((unsigned char *)&buf, 32);

	switch( FWDN_DeviceInformation.DevSerialNumberType )
	{
		case SN_VALID_16: return 16;
		case SN_VALID_32: return 32;
		case SN_INVALID_16: return -16;
		case SN_INVALID_32: return -32;
	}
	return -1;
}

/*
static int __do_mmc_update( page_t *p )
{
	page_t q;
	unsigned char *buf;

	if( DISK_Ioctl( DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_WRITE_PAGE, (void*)p ) )
		return -1;

	buf = p->buff + ((p->rw_size) << 9);
	mmc_init_page( &q, p->start_page, p->rw_size, p->hidden_index, buf, p->boot_partition );

	if( DISK_Ioctl( DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_READ_PAGE, (void*)&q ) ||
			memcmp( p->buff, q.buff, (p->rw_size) << 9 ) )
		return -1;
	return 0;
}
*/

int mmc_update_rom_code(
		unsigned long rom_size,
		unsigned long config_size,
		unsigned char *buf,
		unsigned int boot_partition)
{
	page_t p;

	mmc_init_page(
			&p,
			BOOTSD_GetROMAddr(rom_size, config_size+CRC32_SIZE, boot_partition),
			BYTE_TO_SECTOR(rom_size),
			0,
			buf,
			boot_partition
			);

	if( __do_mmc_update( &p ) ) {
		LPRINTF("[FWDN\t] Write Code Failed\n");
		return ERR_FWUG_FAIL_CODEWRITETFLASH;
	}
	LPRINTF("[FWDN\t] Write Code Completed\n");
	return SUCCESS;
}

int mmc_update_config_code(
		unsigned long rom_size, // ks_8930
		unsigned long config_size,
		unsigned char *buf,
		unsigned int boot_partition)
{
	page_t  p;
	unsigned long crc;
	unsigned char *p_buf = (unsigned char*)global_buf;

	memset( p_buf, 0, FWDN_WRITE_BUFFER_SIZE );
	memcpy( p_buf, buf, config_size );

	crc = FWUG_CalcCrc8( (unsigned char*)p_buf, config_size);
	word_of(p_buf + config_size) = crc;
	config_size += CRC32_SIZE;

	mmc_init_page(
			&p,
			BOOTSD_GetConfigCodeAddr(rom_size, config_size, boot_partition), // ks_8930
			BYTE_TO_SECTOR(config_size),
			0,
			p_buf,
			boot_partition
			);

	if( __do_mmc_update( &p ) )
	{
		LPRINTF("[FWDN\t] Write Config Code Failed\n");
		return ERR_FWUG_FAIL_HEADERWRITETFLASH;
	}
	LPRINTF("[FWDN\t] Write Config Code Completed\n");
	return SUCCESS;
}

int mmc_update_header (
		unsigned long firmware_base,
		unsigned long config_size,
		unsigned long rom_size,
		unsigned int boot_partition,
		int fwdn_mode)
{
	page_t p;
	BOOTSD_Header_Info_T header;
	unsigned char *pTempData   = (unsigned char*)global_buf;

	memset(&header, 0, sizeof(BOOTSD_Header_Info_T));
	memset(pTempData, 0x00, FWDN_WRITE_BUFFER_SIZE);

	header.ulEntryPoint = firmware_base;
	header.ulBaseAddr = firmware_base;
	header.ulConfSize = config_size + CRC32_SIZE;     //CRC plus
	header.ulROMFileSize = rom_size;
	header.ulBootConfig = (BOOTCONFIG_NORMAL_SPEED | BOOTCONFIG_4BIT_BUS);


	header.ulConfigCodeBase = BOOTSD_GetBootStart(boot_partition) +
	BOOTSD_GetConfigCodeAddr(rom_size, config_size+CRC32_SIZE, boot_partition);

	header.ulBootloaderBase = BOOTSD_GetBootStart(boot_partition) +
	BOOTSD_GetROMAddr(rom_size, config_size+CRC32_SIZE, boot_partition);

#if !defined(FEATURE_SDMMC_MMC43_BOOT)
	if( header.ulBootConfig & BOOTCONFIG_8BIT_BUS)
	{
		LPRINTF("[FWDN\t] SD Boot can't support 8 bit data rate\n");
		return -1;
	}
#endif
	{
		sHidden_Size_T stHidden;
		BOOTSD_Hidden_Info(&stHidden);
		header.ulHiddenStartAddr = stHidden.ulHiddenStart[0] + stHidden.ulSectorSize[0];
		memcpy( (void *)header.ulHiddenSize, (const void*)stHidden.ulSectorSize, (BOOTSD_HIDDEN_MAX_COUNT << 2));
	}

	memcpy(header.ucHeaderFlag, "HEAD", 4);

	if( fwdn_mode && g_uiFWDN_WriteSNFlag == 1 )
	{
		unsigned int crc;
		mmc_init_page( &p, BOOTSD_GetHeaderAddr( boot_partition ), 1, 0, pTempData, boot_partition);//ks_8930
		DISK_Ioctl(DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_READ_PAGE, (void*)&p);
		crc = FWUG_CalcCrc8((unsigned char*) &header, 512-4);
		if( word_of(pTempData + 512 -4) != crc)
			return -1;
		memcpy( header.ucSerialNum, pTempData+16, 64 );
	} else if( !fwdn_mode )
		mmc_get_serial_num( header.ucSerialNum, boot_partition );

	FWDN_FNT_InsertSN( header.ucSerialNum );                //Write Serial number

	header.ulCRC = FWUG_CalcCrc8((unsigned char*) &header, 512-4);

	memcpy(pTempData, &header, 512);
	mmc_init_page( &p, BOOTSD_GetHeaderAddr( boot_partition ), 1, 0, pTempData, boot_partition);

	if( __do_mmc_update( &p ) )
		return ERR_FWUG_FAIL_HEADERWRITETFLASH;
	LPRINTF("[FWDN\t] Write Header Completed!!\n");
	return SUCCESS;
}

extern unsigned long InitRoutine_Start;
extern unsigned long InitRoutine_End;

#ifdef CONFIG_USB_UPDATE
extern char *buf_addr_for_bootloader;
#endif

int mmc_update_bootloader(unsigned int rom_size, unsigned int boot_partition)
{
#ifdef CONFIG_USB_UPDATE
	unsigned long scratch_addr = (unsigned long)buf_addr_for_bootloader;
	unsigned long *mem_init_start = (unsigned long*)(scratch_addr + 0x70); /* SCRATCH + 0x70 */
	unsigned long *mem_init_end = (unsigned long*)(scratch_addr + 0x74); /* SCRATCH + 0x74 */
	unsigned long *firmware_base = (unsigned long*)(scratch_addr + 0x60); /* SCRATCH + 0x60 */
	unsigned long mem_init_offset = (*mem_init_start) - (*firmware_base);
	unsigned long init_start = scratch_addr + mem_init_offset;
	unsigned int mem_init_code_size = *mem_init_end - *mem_init_start;;

	printf("[BOOTLOADER UPDATE] Boot partition : %d\n", boot_partition);
#else

  unsigned int config_size = (unsigned int)&InitRoutine_End - (unsigned int)&InitRoutine_Start;

  unsigned long *mem_init_start = (unsigned long *)CONFIG_CODE_START_ADDRESS; /* SCRATCH + 0x70 */
  unsigned long *mem_init_end = (unsigned long *) CONFIG_CODE_END_ADDRESS; /* SCRATCH + 0x74 */
  unsigned long *firmware_base = (unsigned long *)FIRMWARE_BASE_ADDRESS; /* SCRATCH + 0x60 */

  unsigned long mem_init_offset = (*mem_init_start) - (*firmware_base);
  unsigned long init_start = ROMFILE_TEMP_BUFFER + mem_init_offset;

  if (!check_fwdn_mode()) {
    config_size = *mem_init_end - *mem_init_start;
#if 0
    printf(" mem_init_start(0x%08x), *mem_init_start(0x%08x)\n", mem_init_start, *mem_init_start);
    printf(" mem_init_end(0x%08x), *mem_init_end(0x%08x)\n", mem_init_end, *mem_init_end);
    printf(" firmware_base(0x%08x), *firmware_base(0x%08x)\n", firmware_base, *firmware_base);
    printf(" mem_init_offset_rom(0x%08x)\n", mem_init_offset_rom);
    printf(" init_start(0x%08x)\n", init_start);
#endif
  }
#endif

  if( mmc_update_valid_test(rom_size) )
    return -1;

#if defined(FEATURE_SDMMC_MMC43_BOOT)
  LPRINTF("[FWDN\t] Begin eMMC Bootloader write to T-Flash\n");
#else
  LPRINTF("[FWDN\t] Begin SD Bootloader write to T-Flash\n");
#endif

#ifdef CONFIG_USB_UPDATE
  if( mmc_update_header(*firmware_base, mem_init_code_size, rom_size, boot_partition, 0) ) {
	  printf("[BOOTLOADER UPDATE] failed to do mmc_update_header()\n");
	  return -1;
  }
  if( mmc_update_config_code(rom_size, mem_init_code_size, (unsigned char*)init_start, boot_partition) ) {
	  printf("[BOOTLOADER UPDATE] failed to do mmc_update_config_code()\n");
	  return -1;
  }
  if( mmc_update_rom_code(rom_size, mem_init_code_size, (unsigned char*)scratch_addr, boot_partition) ) {
	  printf("[BOOTLOADER UPDATE] failed to do mmc_update_rom_code()\n");
	  return -1;
  }

  LPRINTF("[BOOTLOADER UPDATE] Bootloader update completed.\n");
#else
#if defined(FEATURE_SDMMC_MMC43_BOOT)
  LPRINTF("[FWDN\t] Begin eMMC Bootloader write to T-Flash\n");
#else
  LPRINTF("[FWDN\t] Begin SD Bootloader write to T-Flash\n");
#endif

  if( mmc_update_header(MEMBASE, config_size, rom_size, boot_partition, 0) )
	  return -1;
  if( mmc_update_config_code(rom_size, config_size, (unsigned char*)&InitRoutine_Start, boot_partition) ) //ks_8930
	  return -1;
  if( mmc_update_rom_code(rom_size, config_size, (unsigned char*)ROMFILE_TEMP_BUFFER, boot_partition) )
	  return -1;

  LPRINTF("[FWDN\t] Updating Bootloader Complete!\n");
#endif

#if 0
  if( mmc_update_header(MEMBASE, config_size, rom_size, boot_partition, 0) )
    return -1;
  if (!check_fwdn_mode()) {
    if( mmc_update_config_code(rom_size, config_size, (unsigned char*)init_start, boot_partition) )
      return -1;
  } else {
    if( mmc_update_config_code(rom_size, config_size,
	  (unsigned char*)&InitRoutine_Start, boot_partition) )
      return -1;
  }
  if( mmc_update_rom_code(rom_size, config_size, (unsigned char*)ROMFILE_TEMP_BUFFER,
	boot_partition) )
    return -1;

  LPRINTF("[FWDN\t] Updating Bootloader Complete!\n");
  return SUCCESS;
#endif
  return SUCCESS;
}

#ifdef CONFIG_FASTBOOT_FLASH_MMC_DEV
int fb_mmc_update_bootloader(void *buffer, unsigned int bytes, int partition)
{
	unsigned char *data = buffer;
	unsigned long *mem_init_start =
		(unsigned long *) (data + ROM_CONFIG_CODE_START_OFFSET);
	unsigned long *mem_init_end =
		(unsigned long *) (data + ROM_CONFIG_CODE_END_OFFSET);
	unsigned int config_size =
		(unsigned int) *mem_init_end - *mem_init_start;
	unsigned long *firmware_base =
		(unsigned long *) (data + ROM_FIRMWARE_BASE_OFFSET);
	unsigned long mem_init_offset = *mem_init_start - *firmware_base;
	unsigned char *init_start = data + mem_init_offset;

	if (mmc_update_header(*firmware_base, config_size, bytes, partition, 0))
		return -EIO;
	if (mmc_update_config_code(bytes, config_size, init_start, partition))
		return -EIO;
	if (mmc_update_rom_code(bytes, config_size, data, partition))
		return -EIO;
	return 0;
}
#endif /* CONFIG_FASTBOOT_FLASH_MMC_DEV */

unsigned int FwdnUpdateReadBootSDFirmware(unsigned int master)
{
	return 0;
}

int FwdnUpdateSetBootSDSerial(unsigned char *ucData, unsigned int overwrite)
{
	ioctl_diskinfo_t  disk_info;
	unsigned char   ucTempData[512];
	int       iRev;
	ioctl_diskrwpage_t  header;

	if (DISK_Ioctl(DISK_DEVICE_TRIFLASH, DEV_GET_DISKINFO, (void *)&disk_info) < 0)
	{
		return -1;
	}
	if (disk_info.sector_size != 512)
		return -1;

	memcpy( FWDN_DeviceInformation.DevSerialNumber, ucData , 32);
	g_uiFWDN_OverWriteSNFlag  =  overwrite;
	g_uiFWDN_WriteSNFlag    = 0;

	if ( overwrite == 0 )
	{
		/*----------------------------------------------------------------
		 *       G E T S E R I A L N U M B E R
		 *             ----------------------------------------------------------------*/
		header.start_page = BOOTSD_GetHeaderAddr( 0 );
		header.rw_size    = 1;
		header.buff     = ucTempData;
		iRev  = DISK_Ioctl(DISK_DEVICE_TRIFLASH, DEV_BOOTCODE_READ_PAGE, (void *)&header);
		if(iRev != SUCCESS)
			return (-1);

		iRev = FWDN_FNT_SetSN(ucTempData, 32);
	}
	return 0;
}

