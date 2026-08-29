/* regtable on a NUCLEO-L073RZ: the board's register table on the
 * ST-Link virtual COM port (USART2 on PA2/PA3, 115200 8N1). Open the
 * port in any terminal and type `help`, `list`, `set led true`,
 * `get led_pin`, `fetch`; the same port answers `--json` for the
 * Python client (`regtable connect -p <port>`).
 *
 * The table is generated from regs.yaml (gen/registers.c): the LED,
 * the button, the uptime, and three silicon registers of the
 * STM32L0x3 read in place. This file is the whole application: the
 * clock, the pins, the UART, and a loop that feeds received bytes to
 * the CLI. The UART is polled both ways, without interrupts; the
 * transmit side writes on TXE so echoing overlaps receiving. */

#include "stm32l0xx_hal.h"

#include "regtable_core.h"
#include "regtable_cli.h"
#include "registers.h"

static UART_HandleTypeDef uart;

/* -- hooks the table names (regs.yaml) ---------------------- */

void led_changed(const RegEntry *e)
{
    (void)e;
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, led ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void button_read(const RegEntry *e)
{
    (void)e;
    button = (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET); /* B1 pulls the pin low */
}

void uptime_read(const RegEntry *e)
{
    (void)e;
    uptime = HAL_GetTick();
}

/* -- board bring-up ----------------------------------------- */

/* 32 MHz: the 16 MHz HSI through the PLL (x4 / 2) */
static void clock_init(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = 0x10;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLMUL          = RCC_PLLMUL_4;
    osc.PLL.PLLDIV          = RCC_PLLDIV_2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        for (;;) { }
    }

    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1) != HAL_OK) {
        for (;;) { }
    }
}

static void gpio_init(void)
{
    GPIO_InitTypeDef g = { 0 };
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    g.Pin   = GPIO_PIN_5;               /* LD2 */
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &g);
    g.Pin  = GPIO_PIN_13;               /* B1, external pull-up on the board */
    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOC, &g);
    __HAL_RCC_DBGMCU_CLK_ENABLE();      /* dbgmcu_idcode is read in place */
}

/* HAL_UART_Init calls this back: route USART2 to the VCP pins */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef g = { 0 };
    (void)huart;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    g.Pin       = GPIO_PIN_2 | GPIO_PIN_3;   /* PA2 TX, PA3 RX */
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF4_USART2;
    HAL_GPIO_Init(GPIOA, &g);
}

static void uart_init(void)
{
    uart.Instance        = USART2;
    uart.Init.BaudRate   = 115200;
    uart.Init.WordLength = UART_WORDLENGTH_8B;
    uart.Init.StopBits   = UART_STOPBITS_1;
    uart.Init.Parity     = UART_PARITY_NONE;
    uart.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
    uart.Init.Mode       = UART_MODE_TX_RX;
    if (HAL_UART_Init(&uart) != HAL_OK) {
        for (;;) { }
    }
}

/* Write on TXE, not on transfer-complete: the UART's transmit side
 * is double-buffered (TDR plus the shifter), so the echo of one
 * received byte overlaps the arrival of the next and polling keeps
 * up with a full line sent back-to-back. */
static int vcom_tx(const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        while (!(USART2->ISR & USART_ISR_TXE)) { }
        USART2->TDR = buf[i];
    }
    return (int)len;
}

/* HAL_Init started the 1 ms tick; keep it counting */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* -- main ---------------------------------------------------- */

int main(void)
{
    HAL_Init();
    clock_init();
    gpio_init();
    uart_init();

    static RegTable table;
    static RegCli   cli;
    reg_table_init(&table, nucleo_registry);
    RegTransport tx = { 0 };
    tx.write = vcom_tx;
    regcli_init(&cli, &table, tx);
    static const RegIdentity who = {
        REGTABLE_GEN_NUCLEO_NAME, "1.0", NULL, REGTABLE_GEN_NUCLEO_CHIP,
    };
    regcli_set_identity(&cli, &who);

    for (;;) {
        while (USART2->ISR & USART_ISR_RXNE) {
            regcli_feed(&cli, (uint8_t)USART2->RDR);
        }
        if (USART2->ISR & USART_ISR_ORE) {
            USART2->ICR = USART_ICR_ORECF;      /* a byte lost: start the line over */
        }
        reg_poll(&table);
    }
}
