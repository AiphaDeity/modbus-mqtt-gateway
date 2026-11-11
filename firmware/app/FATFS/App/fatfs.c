/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file   fatfs.c
  * @brief  Code for fatfs applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
#include "fatfs.h"

uint8_t retSD;    /* Return value for SD */
char SDPath[4];   /* SD logical drive path */
FATFS SDFatFS;    /* File system object for SD logical drive */
FIL SDFile;       /* File object for SD */

/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

void MX_FATFS_Init(void)
{
  /*## FatFS: Link the SD driver ###########################*/
  retSD = FATFS_LinkDriver(&SD_Driver, SDPath);

  /* USER CODE BEGIN Init */
  /* additional user code for init */
  /* USER CODE END Init */
}

/**
  * @brief  Gets Time from RTC
  * @param  None
  * @retval Time in DWORD
  */
DWORD get_fattime(void)
{
  /* USER CODE BEGIN get_fattime */
  /*
   * FatFs 时间打包：
   *   bit 31-25 : year - 1980 (7 bits, range 0~127 → 1980~2107)
   *   bit 24-21 : month (4 bits, 1~12)
   *   bit 20-16 : day   (5 bits, 1~31)
   *   bit 15-11 : hour  (5 bits, 0~23)
   *   bit 10-5  : minute(6 bits, 0~59)
   *   bit 4-0   : second/2 (5 bits, 0~29 对应 0~58)
   */
  uint32_t t = storage_get_unix_time();
  if (t <= 0) return 0;   /* 时间未知时 FatFs 会用默认值 */

  /* Unix 时间戳拆分（64 位足够） */
  uint64_t seconds = t;
  uint32_t year  = 1970;
  uint32_t month = 1;
  uint32_t day   = 1;

  /* 年 */
  while (1) {
    uint64_t days_in_year = 365 + ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0 ? 1 : 0);
    if (seconds < days_in_year * 86400ULL) break;
    seconds -= days_in_year * 86400ULL;
    year++;
  }

  /* 月 */
  static const uint8_t month_days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  while (1) {
    uint8_t md = month_days[month - 1] + (month == 2 && leap ? 1 : 0);
    if (seconds < md * 86400ULL) break;
    seconds -= md * 86400ULL;
    month++;
  }

  /* 日 */
  day = (uint32_t)(seconds / 86400ULL) + 1;
  seconds %= 86400ULL;

  uint32_t hh = (uint32_t)(seconds / 3600ULL);
  uint32_t mm = (uint32_t)((seconds % 3600ULL) / 60ULL);
  uint32_t ss = (uint32_t)(seconds % 60ULL);

  if (year < 1980 || year > 2107) return 0;

  return ((DWORD)(year - 1980) << 25) |
         ((DWORD)month          << 21) |
         ((DWORD)day            << 16) |
         ((DWORD)hh             << 11) |
         ((DWORD)mm             << 5)  |
         ((DWORD)(ss / 2));
  /* USER CODE END get_fattime */
}

/* USER CODE BEGIN Application */

/* USER CODE END Application */
