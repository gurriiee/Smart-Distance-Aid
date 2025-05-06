#include "stm32f10x.h"
#include <stdio.h>
#include <stdarg.h>

// === Global Variables ===
volatile uint32_t start = 0, end = 0;
volatile uint8_t isFirstCaptured = 0;

volatile uint32_t msTicks = 0;
volatile uint32_t led_delay = 0, led_timer = 0;
volatile uint32_t buzzer_delay = 0, buzzer_timer = 0;

float previous_distance = 0;
uint8_t sudden_drop_count = 0;

// === Function Prototypes ===
void SystemInit(void);
void gpio_init(void);
void uart1_init(void);
void tim2_input_capture_init(void);
void delay_us(uint32_t us);
void trig_pulse(void);
void uart1_write_char(char c);
int uart_printf(const char *format, ...);
void TIM2_IRQHandler(void);
void SysTick_Handler(void);
void led_blink(void);
void buzzer_beep(void);
void play_startup_pattern(void);

// === MAIN ===
int main(void)
{
    SystemInit();
    gpio_init();
    uart1_init();
    tim2_input_capture_init();
    SysTick_Config(SystemCoreClock / 1000);

    uart_printf("Ultrasonic Sensor & IR Initialized\r\n");
    uart_printf("System ready.\r\n");
    play_startup_pattern();

    while (1)
    {
        uint8_t ir_detected = ((GPIOA->IDR & (1 << 5)) == 0);

        if (ir_detected)
        {
            uart_printf("IR Sensor: Object detected nearby!\r\n");
        }

        isFirstCaptured = 0;
        start = end = 0;

        TIM2->SR = 0;
        TIM2->CNT = 0;
        TIM2->CCER &= ~TIM_CCER_CC2P;
        TIM2->DIER |= TIM_DIER_CC2IE;

        trig_pulse();
        for (volatile int i = 0; i < 800000; i++); // ~50ms

        if (start != 0 && end > start)
        {
            uint32_t pulse_length = end - start;
            float distance_cm = (pulse_length * 0.034f) / 2.0f;

            if (distance_cm > 500.0f || distance_cm == 0.0f)
            {
                uart_printf("Distance too far or echo error. Ignored.\r\n");
                continue;
            }

            // Glitch filtering
            if (previous_distance > 100.0f && distance_cm < 80.0f)
            {
                sudden_drop_count++;
                if (sudden_drop_count < 3)
                {
                    uart_printf("Glitch: Sudden drop detected. Ignored.\r\n");
                    distance_cm = previous_distance;
                }
                else
                {
                    sudden_drop_count = 0;
                }
            }
            else
            {
                sudden_drop_count = 0;
            }

            // Ghost detection via IR
            if (!ir_detected && distance_cm > 80.0f)
            {
                uart_printf("Ghost? IR not triggered. Ignoring ultrasonic (>80cm).\r\n");
                continue;
            }

            previous_distance = distance_cm;
            uart_printf("Distance: %.2f cm\r\n", distance_cm);

            // === Distance-based Response ===
            if (distance_cm > 46.90f)
            {
                GPIOA->BRR = (1 << 6) | (1 << 7) | (1 << 4); // LED, Buzzer, Motor OFF
                led_delay = buzzer_delay = 0;
            }
            else if (distance_cm > 35.0f)
            {
                led_delay = buzzer_delay = 300;
                GPIOA->BSRR = (1 << 4); // Motor ON
            }
            else if (distance_cm > 27.0f)
            {
                led_delay = buzzer_delay = 200;
                GPIOA->BSRR = (1 << 4);
            }
            else if (distance_cm > 20.0f)
            {
                led_delay = buzzer_delay = 100;
                GPIOA->BSRR = (1 << 4);
            }
            else if (distance_cm > 13.0f)
            {
                led_delay = buzzer_delay = 50;
                GPIOA->BSRR = (1 << 4);
            }
            else if (distance_cm >= 2.0f)
            {
                led_delay = buzzer_delay = 25;
                GPIOA->BSRR = (1 << 4);
            }
            else
            {
                GPIOA->BRR = (1 << 4); // Motor OFF below 2cm
            }
        }
        else
        {
            uart_printf("No echo detected.\r\n");
            GPIOA->BRR = (1 << 6) | (1 << 7) | (1 << 4);
            led_delay = buzzer_delay = 0;
        }

        led_blink();
        buzzer_beep();
        for (volatile int i = 0; i < 300000; i++);
    }
}

// === SysTick ISR ===
void SysTick_Handler(void)
{
    msTicks++;

    if (led_delay && (msTicks - led_timer >= led_delay))
    {
        GPIOA->ODR ^= (1 << 6); // LED toggle
        led_timer = msTicks;
    }

    if (buzzer_delay && (msTicks - buzzer_timer >= buzzer_delay))
    {
        GPIOA->ODR ^= (1 << 7); // Buzzer toggle
        buzzer_timer = msTicks;
    }
}

// === LED & Buzzer Control ===
void led_blink(void)
{
    if (!led_delay) GPIOA->BRR = (1 << 6);
}
void buzzer_beep(void)
{
    if (!buzzer_delay) GPIOA->BRR = (1 << 7);
}

// === GPIO Setup ===
void gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;

    // PA0 = Trigger
    GPIOA->CRL &= ~(0xF << 0);
    GPIOA->CRL |= (0x3 << 0); // Output push-pull

    // PA1 = Echo
    GPIOA->CRL &= ~(0xF << 4);
    GPIOA->CRL |= (0x8 << 4); // Input with AF

    // PA4 = Vibration Motor
    GPIOA->CRL &= ~(0xF << 16);
    GPIOA->CRL |= (0x1 << 16); // Output push-pull, 10 MHz
    GPIOA->BRR = (1 << 4);     // ?? Ensure motor OFF at startup

    // PA5 = IR Sensor Input with pull-up
    GPIOA->CRL &= ~(0xF << 20);
    GPIOA->CRL |= (0x4 << 20); // Input mode
    GPIOA->ODR |= (1 << 5);    // Enable pull-up

    // PA6 = LED
    GPIOA->CRL &= ~(0xF << 24);
    GPIOA->CRL |= (0x1 << 24); // Output

    // PA7 = Buzzer
    GPIOA->CRL &= ~(0xF << 28);
    GPIOA->CRL |= (0x1 << 28); // Output

    // ?? Force all outputs OFF at startup
    GPIOA->BRR = (1 << 4) | (1 << 6) | (1 << 7); // Motor, LED, Buzzer OFF
}


// === Trigger Pulse ===
void trig_pulse(void)
{
    GPIOA->BSRR = GPIO_BSRR_BS0;
    delay_us(10);
    GPIOA->BSRR = GPIO_BSRR_BR0;
}

void delay_us(uint32_t us)
{
    us *= 8;
    while (us--) __NOP();
}

// === UART ===
void uart1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPAEN;
    GPIOA->CRH &= ~(0xF << 4);
    GPIOA->CRH |= (0xB << 4); // PA9 = TX

    USART1->BRR = 72000000 / 9600;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

void uart1_write_char(char c)
{
    while (!(USART1->SR & USART_SR_TXE));
    USART1->DR = c;
}

int uart_printf(const char *format, ...)
{
    char buffer[100];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    for (int i = 0; i < len; i++)
        uart1_write_char(buffer[i]);

    return len;
}

// === Timer 2 Input Capture Init ===
void tim2_input_capture_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->PSC = 72 - 1;
    TIM2->ARR = 0xFFFF;
    TIM2->CCMR1 |= TIM_CCMR1_CC2S_0;
    TIM2->CCER |= TIM_CCER_CC2E;
    TIM2->CR1 |= TIM_CR1_CEN;
    NVIC_EnableIRQ(TIM2_IRQn);
}

// === TIM2 IRQ ===
void TIM2_IRQHandler(void)
{
    if (TIM2->SR & TIM_SR_CC2IF)
    {
        if (isFirstCaptured == 0)
        {
            start = TIM2->CCR2;
            isFirstCaptured = 1;
            TIM2->CCER |= TIM_CCER_CC2P;
        }
        else if (isFirstCaptured == 1)
        {
            end = TIM2->CCR2;
            isFirstCaptured = 2;
            TIM2->DIER &= ~TIM_DIER_CC2IE;
        }

        TIM2->SR &= ~TIM_SR_CC2IF;
    }
}

// === Startup Animation ===
void play_startup_pattern(void)
{
    for (int i = 0; i < 3; i++)
    {
        GPIOA->BSRR = (1 << 6) | (1 << 7);
        for (volatile int j = 0; j < 720000; j++);
        GPIOA->BRR = (1 << 6) | (1 << 7);
        for (volatile int j = 0; j < 720000; j++);
    }

    for (volatile int j = 0; j < 2160000; j++);

    for (int i = 0; i < 2; i++)
    {
        GPIOA->BSRR = (1 << 6) | (1 << 7);
        for (volatile int j = 0; j < 720000; j++);
        GPIOA->BRR = (1 << 6) | (1 << 7);
        for (volatile int j = 0; j < 720000; j++);
    }

    GPIOA->BSRR = (1 << 6) | (1 << 7);
    for (volatile int j = 0; j < 2880000; j++);
    GPIOA->BRR = (1 << 6) | (1 << 7);
		

}
