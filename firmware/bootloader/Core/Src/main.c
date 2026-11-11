/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "boot_watchdog.h"
#include <stdio.h>
#include <string.h>
#include "boot_app_flash.h"
#include "boot_package_flash.h"
#include "boot_upgrade_state.h"
#include "boot_trial.h"
#include "boot_recovery.h"
#include "boot_package_receive.h"
#include "boot_install.h"
#include "upgrade_trust.h"
#include "boot_jump.h"
#if defined(BOOT_PACKAGE_RECEIVE_TEST)
#include "boot_test_keys.h"
#endif
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BOOT_STARTUP_TIMEOUT_MS 15000U
#define BOOT_FLASH_TEST_TIMEOUT_MS 10000U
#if defined(DEBUG) || defined(BOOT_PACKAGE_RECEIVE_TEST)
#define BOOT_STARTUP_DIAGNOSTICS 1
#endif

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

#if defined(DEBUG)
// 仅在调试器确认测试区域后手动置 1。
static volatile uint32_t app_flash_test_request = 0U;
#endif

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// 长操作返回后统一检查，监督失败不得转入回退、收包或App交接继续执行。
static void Boot_ResetIfWatchdogFailed(const char *stage)
{
  if (Boot_WatchdogFailed())
  {
    printf("WDG supervision expired stage=%s; reset\r\n", stage);
    __DSB();
    NVIC_SystemReset();
  }
}

#if defined(BOOT_STARTUP_DIAGNOSTICS)
// 启动时用 Mbed TLS 附带的 NIST 向量检查认证解密和标签篡改拒绝。
static void Boot_TestGcm(void)
{
  static const uint8_t key[] =
  {
    0x00U, 0x05U, 0x0AU, 0x21U, 0xCAU, 0x1EU, 0x72U, 0xCDU,
    0x09U, 0x24U, 0xBEU, 0x31U, 0xB9U, 0x43U, 0xC6U, 0x08U,
    0x54U, 0xBEU, 0x67U, 0x44U, 0x57U, 0x7DU, 0xE3U, 0xDDU,
    0x9DU, 0x1FU, 0x4FU, 0xADU, 0xA4U, 0xA1U, 0x9EU, 0xA6U,
  };
  static const uint8_t ciphertext[] =
  {
    0x69U, 0x3FU, 0xFDU, 0x3DU, 0x92U, 0x29U, 0x48U, 0x57U,
    0xA9U, 0x9CU, 0x70U, 0x2AU, 0x07U, 0x99U, 0xEEU, 0xCAU,
    0x28U, 0xABU, 0x06U, 0x6DU, 0xD9U, 0x09U, 0x17U, 0xB9U,
    0xEAU, 0x5EU, 0xF8U, 0xF6U, 0x54U, 0x7FU, 0x1DU, 0x90U,
    0xB1U, 0x06U, 0xCBU, 0xECU, 0x8EU, 0xF2U, 0xC2U, 0x2AU,
    0xF9U, 0xF8U, 0xEFU, 0xA6U, 0xC6U, 0x52U, 0xF2U, 0xF9U,
    0x7CU, 0x2BU, 0xAFU, 0x33U, 0xAFU, 0x14U, 0xFEU, 0x9DU,
    0xEFU, 0x23U, 0x0DU, 0x49U, 0x52U, 0x4BU, 0xD6U, 0x59U,
    0x09U, 0xC3U, 0xDFU, 0x14U, 0x90U, 0xF6U, 0x37U, 0xF9U,
    0x9EU, 0x78U, 0x8DU, 0xCCU, 0x04U, 0x2BU, 0x40U, 0xE0U,
    0x0BU, 0xD5U, 0x24U, 0xC9U, 0x1EU, 0x24U, 0x27U, 0xEFU,
    0x99U, 0x1BU, 0xF7U, 0x7EU, 0x7BU, 0x2FU, 0x77U, 0x0CU,
    0xDAU, 0x6EU, 0x90U, 0x07U, 0x6CU, 0x5DU, 0xACU, 0x4CU,
    0xACU, 0x7EU, 0xE3U, 0x95U, 0x8BU, 0x53U, 0xFFU, 0x8CU,
    0xE8U, 0x46U, 0xC3U, 0xA9U, 0x62U, 0x81U, 0xF5U, 0x3CU,
    0x2CU, 0x52U, 0xF5U, 0xF3U, 0xE5U, 0x23U, 0x53U, 0x6FU,
  };
  static const uint8_t iv[] =
  {
    0x2FU, 0xC1U, 0xAFU, 0xC1U, 0x39U, 0x5DU, 0x84U, 0x09U,
    0x91U, 0x92U, 0x48U, 0x70U, 0x9FU, 0x46U, 0x84U, 0x96U,
  };
  uint8_t tag[] =
  {
    0xE3U, 0x9BU, 0x6AU, 0x7FU, 0xD5U, 0xACU, 0x67U, 0xA2U,
    0xA1U, 0xCCU, 0x24U, 0xD5U, 0xEBU, 0x9DU, 0x9CU, 0x74U,
  };
  static const uint8_t expected_plaintext[] =
  {
    0xCFU, 0xCDU, 0x6BU, 0x9FU, 0xF7U, 0x64U, 0x18U, 0x29U,
    0xCBU, 0xADU, 0xEAU, 0xA2U, 0xE5U, 0x6FU, 0x1FU, 0x15U,
    0x0AU, 0x09U, 0x9EU, 0xCCU, 0xF3U, 0xE3U, 0x78U, 0xFAU,
    0x4DU, 0xA5U, 0x97U, 0x94U, 0xDCU, 0xC4U, 0x49U, 0x0AU,
    0xA4U, 0xF9U, 0xC5U, 0xDBU, 0x0AU, 0xB2U, 0x45U, 0xBEU,
    0xC3U, 0x6AU, 0x7DU, 0x45U, 0x57U, 0xA5U, 0x72U, 0x00U,
    0x8EU, 0x42U, 0xF0U, 0x3BU, 0xC1U, 0xBAU, 0xFFU, 0x3CU,
    0x94U, 0x6FU, 0x23U, 0xF5U, 0x4AU, 0x4DU, 0xC9U, 0x82U,
    0x8FU, 0x10U, 0x6CU, 0xF4U, 0x26U, 0x4EU, 0x4AU, 0xB4U,
    0x01U, 0x65U, 0x83U, 0x9DU, 0x10U, 0x85U, 0xE7U, 0x79U,
    0x5BU, 0x1AU, 0xE0U, 0x95U, 0x0FU, 0x0EU, 0xE4U, 0xA0U,
    0x8EU, 0x46U, 0xADU, 0xA5U, 0x01U, 0xB6U, 0xB5U, 0x1DU,
    0xEEU, 0x0EU, 0x51U, 0x81U, 0x29U, 0xC9U, 0x42U, 0x6EU,
    0x5BU, 0xD4U, 0x4CU, 0x66U, 0x67U, 0x4AU, 0x9FU, 0x99U,
    0xCFU, 0xE6U, 0x76U, 0xF0U, 0x02U, 0xCFU, 0xD3U, 0x44U,
    0xC5U, 0xBBU, 0xD2U, 0x2DU, 0x3DU, 0x91U, 0xE6U, 0x00U,
  };
  uint8_t plaintext[sizeof(ciphertext)]; 
  mbedtls_gcm_context context;

  mbedtls_gcm_init(&context);
  int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 256U);
  if (result == 0)
  {
    result = mbedtls_gcm_auth_decrypt(&context, sizeof(ciphertext), iv, sizeof(iv),
                                     NULL, 0U, tag, sizeof(tag), ciphertext, plaintext);
  }

  if (result != 0)
  {
    printf("GCM valid tag test failed (%d)\r\n", result);
  }
  else if (memcmp(plaintext, expected_plaintext, sizeof(plaintext)) != 0)
  {
    printf("GCM plaintext mismatch\r\n");
  }
  else
  {
    printf("GCM valid tag accepted, plaintext matched\r\n");
    tag[0] ^= 1U;  // 翻转标签一位，检查认证是否拒绝篡改。
    result = mbedtls_gcm_auth_decrypt(&context, sizeof(ciphertext), iv, sizeof(iv),
                                     NULL, 0U, tag, sizeof(tag), ciphertext, plaintext);
    if (result == MBEDTLS_ERR_GCM_AUTH_FAILED)
    {
      printf("GCM modified tag rejected\r\n");
    }
    else
    {
      printf("GCM modified tag test failed (%d)\r\n", result);
    }
  }

  mbedtls_gcm_free(&context);
}

// 由启动流程调用，用固定测试向量检查 P-256 验签及篡改拒绝。
static void Boot_TestEcdsa(void)
{
  // 固定样本使用未压缩公钥、SHA-256 摘要和大端 r、s。
  static const uint8_t public_key_bytes[] =
  {
    0x04U, 0x60U, 0xFEU, 0xD4U, 0xBAU, 0x25U, 0x5AU, 0x9DU,
    0x31U, 0xC9U, 0x61U, 0xEBU, 0x74U, 0xC6U, 0x35U, 0x6DU,
    0x68U, 0xC0U, 0x49U, 0xB8U, 0x92U, 0x3BU, 0x61U, 0xFAU,
    0x6CU, 0xE6U, 0x69U, 0x62U, 0x2EU, 0x60U, 0xF2U, 0x9FU,
    0xB6U, 0x79U, 0x03U, 0xFEU, 0x10U, 0x08U, 0xB8U, 0xBCU,
    0x99U, 0xA4U, 0x1AU, 0xE9U, 0xE9U, 0x56U, 0x28U, 0xBCU,
    0x64U, 0xF2U, 0xF1U, 0xB2U, 0x0CU, 0x2DU, 0x7EU, 0x9FU,
    0x51U, 0x77U, 0xA3U, 0xC2U, 0x94U, 0xD4U, 0x46U, 0x22U,
    0x99U,
  };
  uint8_t digest[] =
  {
    0xAFU, 0x2BU, 0xDBU, 0xE1U, 0xAAU, 0x9BU, 0x6EU, 0xC1U,
    0xE2U, 0xADU, 0xE1U, 0xD6U, 0x94U, 0xF4U, 0x1FU, 0xC7U,
    0x1AU, 0x83U, 0x1DU, 0x02U, 0x68U, 0xE9U, 0x89U, 0x15U,
    0x62U, 0x11U, 0x3DU, 0x8AU, 0x62U, 0xADU, 0xD1U, 0xBFU,
  };
  static const uint8_t signature_r[] =
  {
    0xEFU, 0xD4U, 0x8BU, 0x2AU, 0xACU, 0xB6U, 0xA8U, 0xFDU,
    0x11U, 0x40U, 0xDDU, 0x9CU, 0xD4U, 0x5EU, 0x81U, 0xD6U,
    0x9DU, 0x2CU, 0x87U, 0x7BU, 0x56U, 0xAAU, 0xF9U, 0x91U,
    0xC3U, 0x4DU, 0x0EU, 0xA8U, 0x4EU, 0xAFU, 0x37U, 0x16U,
  };
  static const uint8_t signature_s[] =
  {
    0xF7U, 0xCBU, 0x1CU, 0x94U, 0x2DU, 0x65U, 0x7CU, 0x41U,
    0xD4U, 0x36U, 0xC7U, 0xA1U, 0xB6U, 0xE2U, 0x9FU, 0x65U,
    0xF3U, 0xE9U, 0x00U, 0xDBU, 0xB9U, 0xAFU, 0xF4U, 0x06U,
    0x4DU, 0xC4U, 0xABU, 0x2FU, 0x84U, 0x3AU, 0xCDU, 0xA8U,
  };
  // 自测与真实包走同一分段实现，保留错误码以区分篡改拒绝和内部故障。
  uint8_t signature[64];
  memcpy(signature, signature_r, sizeof(signature_r));
  memcpy(signature + sizeof(signature_r), signature_s, sizeof(signature_s));
  int result = Boot_PackageVerifyDigestResult(digest, signature, public_key_bytes);

  if (result == 0)
  {
    printf("ECDSA valid signature accepted\r\n");

    // 反向测试：篡改必须被拒绝
    digest[0] ^= 1U;
    result = Boot_PackageVerifyDigestResult(digest, signature, public_key_bytes);
    if (result == MBEDTLS_ERR_ECP_VERIFY_FAILED)
    {
      printf("ECDSA modified digest rejected\r\n");
    }
    else
    {
      printf("ECDSA modified digest test failed (%d)\r\n", result);
    }
  }
  else
  {
    printf("ECDSA valid signature test failed (%d)\r\n", result);
  }

}


#endif

#if defined(DEBUG)
// 对空白扇区 7 做一次擦除、写入、读回和清理测试。
static void Boot_TestAppFlash(void)
{
  const uint32_t test_address = 0x08060000U;
  const uint32_t test_end = 0x08080000U;
  const uint8_t pattern[] = {0x12U, 0x34U, 0x56U, 0x78U};
  const uint8_t erased[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};

  for (uint32_t address = test_address; address < test_end; address += 4U)
  {
    if (*(const volatile uint32_t *)address != UINT32_MAX)
    {
      printf("App Flash test skipped: sector 7 is not blank\r\n");
      return;
    }
  }

  if (!Boot_AppFlashEraseSector(FLASH_SECTOR_7))
  {
    printf("App Flash test failed: initial erase\r\n");
    return;
  }

  if (!Boot_AppFlashProgram(test_address, pattern, sizeof(pattern)))
  {
    printf("App Flash test failed: program\r\n");
    return;
  }

  if (!Boot_AppFlashVerify(test_address, pattern, sizeof(pattern)))
  {
    printf("App Flash test failed: verify pattern\r\n");
    return;
  }

  if (!Boot_AppFlashEraseSector(FLASH_SECTOR_7))
  {
    printf("App Flash test failed: cleanup erase\r\n");
    return;
  }

  if (!Boot_AppFlashVerify(test_address, erased, sizeof(erased)))
  {
    printf("App Flash test failed: verify erased bytes\r\n");
    return;
  }

  printf("App Flash test passed: sector 7\r\n");
}
#endif

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  // 复位标志每次Boot消费一次，冷启动/外部复位不接受残留确认消息。
  uint32_t reset_flags = RCC->CSR;
  bool software_reset = ((reset_flags & RCC_CSR_SFTRSTF) != 0U)
                     && ((reset_flags & (RCC_CSR_PORRSTF | RCC_CSR_BORRSTF
                                        | RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) == 0U);
  Boot_TrialBegin((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS, software_reset);
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_SPI2_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
  // 基础外设就绪后启狗，覆盖后续诊断、升级和串口维护。
  if (!Boot_WatchdogStart())
  {
    Error_Handler();
  }
  uint32_t boot_started_ms = HAL_GetTick();
  printf("WDG active before startup diagnostics\r\n");
#endif

  // 发送唤醒命令退出 Flash 深度掉电模式，不修改存储内容。
  uint8_t wake_command = 0xABU;
  HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef wake_status = HAL_SPI_Transmit(&hspi2, &wake_command, 1U, 10U);
  HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_Delay(1U);
  printf("Flash wake: HAL status=%u\r\n", (unsigned int)wake_status);

#if defined(BOOT_STARTUP_DIAGNOSTICS)
  // 输出固定输入 "abc" 的 SHA-256，供启动诊断核对。
  const uint8_t sha_input[] = {'a', 'b', 'c'};
  uint8_t sha_digest[32];

  if (mbedtls_sha256(sha_input, sizeof(sha_input), sha_digest, 0) == 0)
  {
    printf("SHA-256:");
    for (unsigned int i = 0U; i < sizeof(sha_digest); i++)
    {
      printf(" %02X", (unsigned int)sha_digest[i]);
    }
    printf("\r\n");
  }
  else
  {
    printf("SHA-256 failed\r\n");
  }


  Boot_TestEcdsa();
#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
  // 完成一个诊断阶段后报告进展，仍沿用整组15s起点。
  if (!Boot_WatchdogProgress(boot_started_ms, BOOT_STARTUP_TIMEOUT_MS))
  {
    Boot_ResetIfWatchdogFailed("startup-ecdsa");
    Error_Handler();
  }
#endif
  Boot_TestGcm();
#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
  if (!Boot_WatchdogProgress(boot_started_ms, BOOT_STARTUP_TIMEOUT_MS))
  {
    Boot_ResetIfWatchdogFailed("startup-gcm");
    Error_Handler();
  }
#endif

  // 每次独立片选读取 ID，保留所有异常，不以重试成功掩盖先前错误。
  uint8_t tx[4] = {0x9FU, 0x00U, 0x00U, 0x00U};
  uint8_t rx[4];
  uint32_t id_ok = 0U;
  for (uint32_t round = 0U; round < 16U; round++)
  {
    memset(rx, 0, sizeof(rx));
    HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi2, tx, rx, sizeof(tx), 10U);
    HAL_GPIO_WritePin(SPI_FLASH_CS_GPIO_Port, SPI_FLASH_CS_Pin, GPIO_PIN_SET);

    if ((status == HAL_OK) && (rx[1] == 0xEFU) && (rx[2] == 0x40U) && (rx[3] == 0x18U))
    {
      id_ok++;
    }
    printf("JEDEC %lu: HAL=%u ID=%02X %02X %02X\r\n",
           (unsigned long)(round + 1U), (unsigned int)status,
           (unsigned int)rx[1], (unsigned int)rx[2], (unsigned int)rx[3]);
    HAL_Delay(1U);
  }
  printf("JEDEC SUMMARY: correct=%lu/16\r\n", (unsigned long)id_ok);

  // 读取外部 Flash 起始位置的 16 字节，供串口检查。
  uint8_t flash_data[16] = {0U};
  HAL_StatusTypeDef read_status = Boot_FlashRead(0U, flash_data, 16U);

  if (read_status == HAL_OK)
  {
    printf("Flash [0x000000]:");
    for (uint16_t i = 0U; i < 16U; i++)
    {
      printf(" %02X", (unsigned int)flash_data[i]);
    }
    printf("\r\n");
  }
  else
  {
    printf("Flash read failed: HAL status %d\r\n", (int)read_status);
  }

#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
  // 完成诊断后检查整组总期限；期限通过不代表各自测结果通过。
  if (!Boot_WatchdogProgress(boot_started_ms, BOOT_STARTUP_TIMEOUT_MS))
  {
    Boot_ResetIfWatchdogFailed("startup");
    Error_Handler();
  }
#endif

#endif

#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
  if (!Boot_WatchdogProgress(boot_started_ms, BOOT_STARTUP_TIMEOUT_MS))
  {
    Boot_ResetIfWatchdogFailed("startup");
    Error_Handler();
  }
#endif

#if defined(BOOT_FLASH_READ_DIAGNOSTIC)
  Boot_TestFlashReadDiagnostic();
  // 只读诊断结束后留在 Boot，不执行后续擦写测试、串口收包或 App。
  while (1)
  {
    HAL_Delay(1000U);
  }
#endif

#if defined(DEBUG)
  if (app_flash_test_request == 1U)
  {
    app_flash_test_request = 0U;
#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
    uint32_t test_started_ms = HAL_GetTick();
#endif
    Boot_TestAppFlash();
#if !defined(BOOT_FLASH_READ_DIAGNOSTIC)
    if (!Boot_WatchdogProgress(test_started_ms, BOOT_FLASH_TEST_TIMEOUT_MS))
    {
      Boot_ResetIfWatchdogFailed("flash-test");
      Error_Handler();
    }
#endif
  }
#endif

  // 测试根仅由显式测试配置绑定；普通构建只从保护区平台映射取得可信资料。
#if defined(BOOT_PACKAGE_RECEIVE_TEST)
  const UpgradeTrustKeys test_keys = {1U, boot_test_signing_public_key, 1U, boot_test_aes_key};
  if (!UpgradeTrust_Bind(&test_keys))
  {
    Error_Handler();
  }
#else
  (void)UpgradeTrust_Initialize();
#endif
  const uint8_t *public_key = NULL;
  const uint8_t *aes_key = NULL;
  bool keys_ready = UpgradeTrust_Get(1U, 1U, &public_key, &aes_key);
  Boot_InstallResult install_result = keys_ready ? Boot_InstallRun(public_key, aes_key) : BOOT_INSTALL_STATE_ERROR;
  Boot_ResetIfWatchdogFailed("install");
  printf("Install result=%u\r\n", (unsigned int)install_result);
  bool allow_app = (install_result == BOOT_INSTALL_BOOT_EXISTING)
                || (install_result == BOOT_INSTALL_TRIAL_STARTED)
                || (install_result == BOOT_INSTALL_RESTORED);
  allow_app = allow_app && Boot_CheckAppVector();
  bool serial_service = !allow_app;
  // 新安装的试运行只放行一次，不能在同一次启动中再次Run而误判为未确认重启。
  while ((install_result != BOOT_INSTALL_TRIAL_STARTED) || !allow_app)
  {
    BootReceiveResult received = Boot_RecoveryReceive(public_key, aes_key);
    Boot_ResetIfWatchdogFailed("receive");
    if (received == BOOT_RECEIVE_COMMAND)
    {
      serial_service = true;
      continue;
    }
    if (received == BOOT_RECEIVE_NO_REQUEST)
    {
      if (allow_app && !serial_service)
      {
        break;
      }
      HAL_Delay(10U);
      continue;
    }
    install_result = keys_ready ? Boot_InstallRun(public_key, aes_key) : BOOT_INSTALL_STATE_ERROR;
    Boot_ResetIfWatchdogFailed("install");
    Boot_RecoveryReplyResult(install_result);
    allow_app = ((install_result == BOOT_INSTALL_BOOT_EXISTING)
                 || (install_result == BOOT_INSTALL_TRIAL_STARTED)
                 || (install_result == BOOT_INSTALL_RESTORED)) && Boot_CheckAppVector();
    if (allow_app)
    {
      break;
    }
    serial_service = true;
  }
  if (!allow_app)
  {
    printf("Upgrade requires install/restore/recovery; stay in Boot\r\n");
    while (1)
    {
      HAL_Delay(1000U);
    }
  }

  if (Boot_CheckAppVector())
  {
    if (!Boot_TrialPrepareApp((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS))
    {
      Error_Handler();
    }
    printf("App vector valid\r\n");
    // 最终认证通过后交接 App，IWDG 在跳转过程中持续运行。
    if (!Boot_WatchdogPrepareApp(public_key, aes_key))
    {
      Boot_ResetIfWatchdogFailed("handoff");
      Error_Handler();
    }
    if(!Boot_JumpToApp())
    {
      Error_Handler();
    }
  }
  else
  {
    printf("App vector invalid\r\n");
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// 将 printf 的单个字符送往 USART1，发送失败返回 EOF。
int __io_putchar(int ch)
{
  uint8_t data = (uint8_t)ch;
  if (HAL_UART_Transmit(&huart1, &data, 1U, 10U) != HAL_OK)
  {
    return EOF;
  }
  return (int)data;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
