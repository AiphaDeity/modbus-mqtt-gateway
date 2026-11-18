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
#include "cmsis_os.h"
#include "dma.h"
#include "eth.h"
#include "fatfs.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "watchdog.h"
#include <stdio.h>
#include "ethernet.h"   /* Day2：m2_ethernet_init() 声明（bsp/ethernet/ethernet.h） */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  // Boot_JumpToApp 先进入 App Reset_Handler；启动文件完成数据段和 C 运行环境初始化后到达这里。
  // App 主流程：接管 IWDG → 初始化外设 → 建立 RTOS 启动任务 → 调度 App_Entry 装配业务。

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  // 1. HAL 时基建立后立即接管仍在运行的 IWDG，再执行后面的时钟和外设初始化。
  // DEBUG 时允许调试暂停冻结计数；正常运行仍须由后续 Monitor 按健康条件喂狗。
  // 这里重配和重装的是 Boot 留下的同一个 IWDG；早期初始化完成后，刷新资格交给监督任务。
#if defined(DEBUG)
  if (!Watchdog_Start(true))
#else
  if (!Watchdog_Start(false))
#endif
  {
    Error_Handler();
  }

  uint32_t watchdog_peripherals_started_ms = HAL_GetTick();
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  // 2. 时钟配置完成，下面建立 App 所需的外设接口；模块业务尚未开始运行。
  // MX_ETH_Init/MX_FATFS_Init 是底层初始化，实际网络及存储恢复由后续任务处理。

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ETH_Init();
  MX_I2C1_Init();
  MX_SPI3_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  MX_TIM7_Init();
  MX_USART1_UART_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
  setvbuf(stdout, NULL, _IONBF, 0);
  // 仅汇总早期初始化不喂狗区间，不因日志而额外刷新IWDG。
  printf("WDG BUDGET app peripherals elapsed_ms=%lu\r\n",
         (unsigned long)(HAL_GetTick() - watchdog_peripherals_started_ms));
  printf("Gateway boot V133\r\n");
  // Boot 管理启动时已消费并清除复位标志，这里的全零标志不能单独排除 IWDG 复位。
  uint32_t reset_flags = RCC->CSR;
  printf("[BOOT] reset: POR=%d BOR=%d PIN=%d SW=%d IWDG=%d WWDG=%d LPWR=%d CSR=0x%08lX\r\n",
         (reset_flags & RCC_CSR_PORRSTF) ? 1 : 0, (reset_flags & RCC_CSR_BORRSTF) ? 1 : 0,
         (reset_flags & RCC_CSR_PINRSTF) ? 1 : 0, (reset_flags & RCC_CSR_SFTRSTF) ? 1 : 0,
         (reset_flags & RCC_CSR_IWDGRSTF) ? 1 : 0, (reset_flags & RCC_CSR_WWDGRSTF) ? 1 : 0,
         (reset_flags & RCC_CSR_LPWRRSTF) ? 1 : 0, (unsigned long)reset_flags);
  __HAL_RCC_CLEAR_RESET_FLAGS();
  // 3. 接下来 osKernelInitialize 建立内核，MX_FREERTOS_Init 创建 defaultTask，尚未执行任务体。
  // 4. osKernelStart 开始调度，defaultTask 的 StartDefaultTask 再调用 App_Entry 装配业务。
  // 正常执行会转到 freertos.c 的启动任务，不再回到 main 的 while 循环。
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  Error_Handler();
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
  RCC_OscInitStruct.PLL.PLLQ = 7;
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
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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
