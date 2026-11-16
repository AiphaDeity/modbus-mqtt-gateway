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
#include "boot_upgrade_state.h"
#include "boot_trial.h"
#include "boot_recovery.h"
#include "boot_install.h"
#include "upgrade_trust.h"
#include "boot_jump.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BOOT_STARTUP_TIMEOUT_MS 15000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

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

  // 获取上次复位标志位
  uint32_t reset_flags = RCC->CSR;
  // 判断是否为纯软件复位，并且没有发生过掉电、欠压、被看门狗咬死
  bool software_reset = ((reset_flags & RCC_CSR_SFTRSTF) != 0U)
                     && ((reset_flags & (RCC_CSR_PORRSTF | RCC_CSR_BORRSTF
                                        | RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) == 0U);
  // 根据状态和邮箱地址，判断继续运行还是回滚
  Boot_TrialBegin((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS, software_reset);
  // 先清理标志位
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_SPI3_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  // 基础外设就绪后启狗，覆盖 Flash 唤醒、升级和串口维护。
  if (!Boot_WatchdogStart())
  {
    Error_Handler();
  }
  // 记录下本次 Bootloader 启狗成功后的绝对起点，再打印日志
  uint32_t boot_started_ms = HAL_GetTick();
  printf("Boot watchdog active\r\n");

  // 发送 0xAB，唤醒外部 Flash
  uint8_t wake_command = 0xABU;  // 准备退出深度掉电的命令
  // 选中外部 Flash，开始本次通信
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  // 通过 SPI3 发送命令
  HAL_StatusTypeDef wake_status = HAL_SPI_Transmit(&hspi3, &wake_command, 1U, 10U);
  // 拉高结束本次通信
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_Delay(1U);
  // 	输出 HAL 传输状态
  printf("Flash wake: HAL status=%u\r\n", (unsigned int)wake_status);

  if (!Boot_WatchdogProgress(boot_started_ms, BOOT_STARTUP_TIMEOUT_MS))
  {
    Boot_ResetIfWatchdogFailed("startup");
    Error_Handler();
  }

  // 初始化可信资料映射
  (void)UpgradeTrust_Initialize();
  // 准备两个指针变量
  const uint8_t *public_key = NULL;  // 指向65字节验签公钥，用来验证发布方签名
  const uint8_t *aes_key = NULL;  // 指向32字节 AES 密钥，用来认证解密升级包
  // 获是否成功取得对应资料
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
