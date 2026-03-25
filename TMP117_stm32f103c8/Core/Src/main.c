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

// TMP117 default i2c device addr 1001000x
//
// PB6 - I2C SCL
// PB7 - I2C SDA
// PA0 - ADC1_IN0, connect PT1000 / 1.65k divider midpoint here
// PB5 - usb OD reset connected to DP.
// 1k5 R connect DP to 3V3
// PB8 - alert input with ext pull-up
// PB9 - ADD0 output with ext pull-down to GND


/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usb_device.h"
#include "usbd_cdc_if.h"
#include <limits.h>
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TMP117_I2C_ADDRESS     (0x48U << 1)
#define TMP117_REG_TEMPERATURE 0x00U
#define TMP117_I2C_TIMEOUT_MS  50U
#define USB_RECONNECT_DELAY_MS 50U

#define PT1000_FILTER_SAMPLE_RATE_HZ        1000U
#define PT1000_USB_OUTPUT_RATE_HZ           20U
#define PT1000_OUTPUT_TICKS                 (PT1000_FILTER_SAMPLE_RATE_HZ / PT1000_USB_OUTPUT_RATE_HZ)
#define PT1000_FILTER_ALPHA_Q15             1937U /* fc ~= 10 Hz at fs = 1 kHz */
#define PT1000_ADC_FULL_SCALE_COUNTS        4095U
#define PT1000_BOTTOM_RESISTOR_MOHM         1651376LL // 1650000LL
#define PT1000_REFERENCE_RESISTANCE_MOHM    1000000LL
#define PT1000_ALPHA_PPM_PER_C              3850LL

#define USB_STREAM_MODE_PT1000              0U
#define USB_STREAM_MODE_TMP117              1U
#define USB_STREAM_MODE_BOTH                2U
#define USB_STREAM_MODE                     USB_STREAM_MODE_PT1000

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim2;

/* USER CODE BEGIN PV */
static volatile uint8_t usb_frame_due = 0U;
static volatile uint8_t tmp117_sample_due = 0U;
static volatile uint16_t pt1000_filtered_adc_counts = 0U;
static uint8_t usb_tx_pending = 0U;
static uint8_t usb_tx_buffer[96];
static uint16_t usb_tx_length = 0U;
static uint8_t pt1000_filter_initialized = 0U;
static int32_t pt1000_filter_state_q15 = 0;
static int32_t latest_pt1000_milli_c = 0;
static int32_t latest_tmp117_milli_c = 0;
static uint8_t latest_tmp117_valid = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_ADC1_Init(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */
static void USB_ReconnectPulse(void);
static HAL_StatusTypeDef TMP117_ReadRawTemperature(int16_t *raw_temperature);
static int32_t TMP117_RawToMilliC(int16_t raw_temperature);
static HAL_StatusTypeDef PT1000_SampleAndFilter(void);
static int32_t PT1000_AdcToMilliC(uint16_t adc_counts);
static void QueuePT1000Frame(void);
static void QueueTMP117Frame(void);
static void QueueBothTemperaturesFrame(void);
static void QueueSelectedTemperatureFrame(void);
static void ServiceTemperatureStream(void);

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

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  USB_ReconnectPulse();
  MX_USB_DEVICE_Init();

  if (HAL_TIM_Base_Start_IT(&htim2) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    ServiceTemperatureStream();
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
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInit.UsbClockSelection = RCC_USBCLKSOURCE_PLL_DIV1_5;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 72 - 1;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 1000 - 1;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(USB_RESER_DP_1K5_GPIO_Port, USB_RESER_DP_1K5_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(ADD0_GPIO_Port, ADD0_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : USB_RESER_DP_1K5_Pin */
  GPIO_InitStruct.Pin = USB_RESER_DP_1K5_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_RESER_DP_1K5_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : ALERT_Pin */
  GPIO_InitStruct.Pin = ALERT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(ALERT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : ADD0_Pin */
  GPIO_InitStruct.Pin = ADD0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ADD0_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  static uint16_t output_tick_divider = 0U;

  if (htim->Instance == TIM2)
  {
    (void)PT1000_SampleAndFilter();

    output_tick_divider++;
    if (output_tick_divider >= PT1000_OUTPUT_TICKS)
    {
      output_tick_divider = 0U;
      usb_frame_due = 1U;
      tmp117_sample_due = 1U;
    }
  }
}

static void USB_ReconnectPulse(void)
{
  HAL_GPIO_WritePin(USB_RESER_DP_1K5_GPIO_Port, USB_RESER_DP_1K5_Pin, GPIO_PIN_RESET);
  HAL_Delay(USB_RECONNECT_DELAY_MS);
  HAL_GPIO_WritePin(USB_RESER_DP_1K5_GPIO_Port, USB_RESER_DP_1K5_Pin, GPIO_PIN_SET);
  HAL_Delay(USB_RECONNECT_DELAY_MS);
}

static HAL_StatusTypeDef TMP117_ReadRawTemperature(int16_t *raw_temperature)
{
  uint8_t raw_data[2];

  if (HAL_I2C_Mem_Read(&hi2c1, TMP117_I2C_ADDRESS, TMP117_REG_TEMPERATURE,
                       I2C_MEMADD_SIZE_8BIT, raw_data, sizeof(raw_data),
                       TMP117_I2C_TIMEOUT_MS) != HAL_OK)
  {
    return HAL_ERROR;
  }

  *raw_temperature = (int16_t)(((uint16_t)raw_data[0] << 8) | raw_data[1]);
  return HAL_OK;
}

static int32_t TMP117_RawToMilliC(int16_t raw_temperature)
{
  return ((int32_t)raw_temperature * 1000) / 128;
}

static HAL_StatusTypeDef PT1000_SampleAndFilter(void)
{
  uint16_t raw_adc_counts;
  int32_t raw_adc_q15;

  if (HAL_ADC_Start(&hadc1) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (HAL_ADC_PollForConversion(&hadc1, 1U) != HAL_OK)
  {
    (void)HAL_ADC_Stop(&hadc1);
    return HAL_ERROR;
  }

  raw_adc_counts = (uint16_t)HAL_ADC_GetValue(&hadc1);
  (void)HAL_ADC_Stop(&hadc1);

  if (raw_adc_counts == 0U)
  {
    raw_adc_counts = 1U;
  }
  else if (raw_adc_counts >= PT1000_ADC_FULL_SCALE_COUNTS)
  {
    raw_adc_counts = PT1000_ADC_FULL_SCALE_COUNTS - 1U;
  }

  raw_adc_q15 = (int32_t)raw_adc_counts << 15;

  if (pt1000_filter_initialized == 0U)
  {
    pt1000_filter_state_q15 = raw_adc_q15;
    pt1000_filter_initialized = 1U;
  }
  else
  {
    pt1000_filter_state_q15 +=
        (int32_t)(((int64_t)(raw_adc_q15 - pt1000_filter_state_q15) * PT1000_FILTER_ALPHA_Q15) >> 15);
  }

  pt1000_filtered_adc_counts = (uint16_t)((pt1000_filter_state_q15 + (1L << 14)) >> 15);
  return HAL_OK;
}

static int32_t PT1000_AdcToMilliC(uint16_t adc_counts)
{
  uint32_t adc_counts_u32 = adc_counts;
  int64_t pt1000_resistance_mohm;
  int64_t milli_c;

  /*
   * Divider:
   * 3V3 --- PT1000 --- ADC input --- 1.65k --- GND
   *
   * Using Vref = VCC cancels the supply in the resistance calculation:
   * Rpt = Rbottom * (4095 - adc) / adc
   *
   * Temperature uses the PT1000 alpha approximation:
   * R(T) = R0 * (1 + 0.00385 * T)
   */
  if (adc_counts_u32 == 0U)
  {
    adc_counts_u32 = 1U;
  }
  else if (adc_counts_u32 >= PT1000_ADC_FULL_SCALE_COUNTS)
  {
    adc_counts_u32 = PT1000_ADC_FULL_SCALE_COUNTS - 1U;
  }

  pt1000_resistance_mohm =
      (PT1000_BOTTOM_RESISTOR_MOHM * (PT1000_ADC_FULL_SCALE_COUNTS - adc_counts_u32) + (adc_counts_u32 / 2U)) /
      adc_counts_u32;
  milli_c = ((pt1000_resistance_mohm - PT1000_REFERENCE_RESISTANCE_MOHM) * 1000LL) /
            PT1000_ALPHA_PPM_PER_C;

  if (milli_c > INT32_MAX)
  {
    return INT32_MAX;
  }
  if (milli_c < INT32_MIN)
  {
    return INT32_MIN;
  }

  return (int32_t)milli_c;
}

static void QueuePT1000Frame(void)
{
  int length;
  uint32_t absolute_milli_c;

  absolute_milli_c = (uint32_t)((latest_pt1000_milli_c < 0) ? -latest_pt1000_milli_c : latest_pt1000_milli_c);
  length = snprintf((char *)usb_tx_buffer, sizeof(usb_tx_buffer),
                    "PT1000=%s%lu.%03lu C\r\n",
                    (latest_pt1000_milli_c < 0) ? "-" : "",
                    (unsigned long)(absolute_milli_c / 1000U),
                    (unsigned long)(absolute_milli_c % 1000U));

  if (length < 0)
  {
    return;
  }
  if ((size_t)length >= sizeof(usb_tx_buffer))
  {
    length = (int)sizeof(usb_tx_buffer) - 1;
  }
  usb_tx_length = (uint16_t)length;
  usb_tx_pending = 1U;
}

static void QueueTMP117Frame(void)
{
  int length;
  uint32_t absolute_milli_c;

  if (latest_tmp117_valid == 0U)
  {
    usb_tx_length = (uint16_t)snprintf((char *)usb_tx_buffer, sizeof(usb_tx_buffer),
                                       "TMP117=error\r\n");
    usb_tx_pending = 1U;
    return;
  }

  absolute_milli_c = (uint32_t)((latest_tmp117_milli_c < 0) ? -latest_tmp117_milli_c
                                                             : latest_tmp117_milli_c);

  length = snprintf((char *)usb_tx_buffer, sizeof(usb_tx_buffer),
                    "TMP117=%s%lu.%03lu C\r\n",
                    (latest_tmp117_milli_c < 0) ? "-" : "",
                    (unsigned long)(absolute_milli_c / 1000U),
                    (unsigned long)(absolute_milli_c % 1000U));

  if (length < 0)
  {
    return;
  }

  if ((size_t)length >= sizeof(usb_tx_buffer))
  {
    length = (int)sizeof(usb_tx_buffer) - 1;
  }

  usb_tx_length = (uint16_t)length;
  usb_tx_pending = 1U;
}

static void QueueBothTemperaturesFrame(void)
{
  int length;
  uint32_t pt1000_absolute_milli_c;
  uint32_t tmp117_absolute_milli_c;

  if (latest_tmp117_valid == 0U)
  {
    pt1000_absolute_milli_c =
        (uint32_t)((latest_pt1000_milli_c < 0) ? -latest_pt1000_milli_c : latest_pt1000_milli_c);
    length = snprintf((char *)usb_tx_buffer, sizeof(usb_tx_buffer),
                      "PT1000=%s%lu.%03lu C TMP117=error\r\n",
                      (latest_pt1000_milli_c < 0) ? "-" : "",
                      (unsigned long)(pt1000_absolute_milli_c / 1000U),
                      (unsigned long)(pt1000_absolute_milli_c % 1000U));
  }
  else
  {
    pt1000_absolute_milli_c =
        (uint32_t)((latest_pt1000_milli_c < 0) ? -latest_pt1000_milli_c : latest_pt1000_milli_c);
    tmp117_absolute_milli_c =
        (uint32_t)((latest_tmp117_milli_c < 0) ? -latest_tmp117_milli_c : latest_tmp117_milli_c);

    length = snprintf((char *)usb_tx_buffer, sizeof(usb_tx_buffer),
                      "PT1000=%s%lu.%03lu C TMP117=%s%lu.%03lu C\r\n",
                      (latest_pt1000_milli_c < 0) ? "-" : "",
                      (unsigned long)(pt1000_absolute_milli_c / 1000U),
                      (unsigned long)(pt1000_absolute_milli_c % 1000U),
                      (latest_tmp117_milli_c < 0) ? "-" : "",
                      (unsigned long)(tmp117_absolute_milli_c / 1000U),
                      (unsigned long)(tmp117_absolute_milli_c % 1000U));
  }

  if (length < 0)
  {
    return;
  }

  if ((size_t)length >= sizeof(usb_tx_buffer))
  {
    length = (int)sizeof(usb_tx_buffer) - 1;
  }

  usb_tx_length = (uint16_t)length;
  usb_tx_pending = 1U;
}

static void QueueSelectedTemperatureFrame(void)
{
  /*
   * USB stream selection:
   * - USB_STREAM_MODE_PT1000 = send only PT1000 temperature
   * - USB_STREAM_MODE_TMP117 = send only TMP117 temperature
   * - USB_STREAM_MODE_BOTH   = send both sensors in one line (current default)
   *
   * Change USB_STREAM_MODE in the private defines section above.
   * The PC viewer applies its own tunable LPF in software.
   */
  switch (USB_STREAM_MODE)
  {
    case USB_STREAM_MODE_TMP117:
      QueueTMP117Frame();
      break;

    case USB_STREAM_MODE_BOTH:
      QueueBothTemperaturesFrame();
      break;

    case USB_STREAM_MODE_PT1000:
    default:
      QueuePT1000Frame();
      break;
  }
}

static void ServiceTemperatureStream(void)
{
  uint8_t run_usb_frame = 0U;
  uint8_t run_tmp117_measurement = 0U;
  uint16_t filtered_adc_counts;

  __disable_irq();
  if (tmp117_sample_due != 0U)
  {
    tmp117_sample_due = 0U;
    run_tmp117_measurement = 1U;
  }

  if ((usb_tx_pending == 0U) && (usb_frame_due != 0U))
  {
    usb_frame_due = 0U;
    run_usb_frame = 1U;
  }
  filtered_adc_counts = pt1000_filtered_adc_counts;
  __enable_irq();

  if (run_tmp117_measurement != 0U)
  {
    int16_t raw_temperature;

    if (TMP117_ReadRawTemperature(&raw_temperature) == HAL_OK)
    {
      latest_tmp117_milli_c = TMP117_RawToMilliC(raw_temperature);
      latest_tmp117_valid = 1U;
    }
    else
    {
      latest_tmp117_valid = 0U;
    }
  }

  if (run_usb_frame != 0U)
  {
    latest_pt1000_milli_c = PT1000_AdcToMilliC(filtered_adc_counts);
    QueueSelectedTemperatureFrame();
  }

  if ((usb_tx_pending != 0U) && (CDC_Transmit_FS(usb_tx_buffer, usb_tx_length) == USBD_OK))
  {
    usb_tx_pending = 0U;
  }
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
