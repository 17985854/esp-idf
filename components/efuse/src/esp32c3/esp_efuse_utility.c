/*
 * SPDX-FileCopyrightText: 2017-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/param.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "assert.h"
#include "esp_efuse_utility.h"
#include "soc/efuse_periph.h"
#include "esp32c3/clk.h"
#include "esp32c3/rom/efuse.h"

static const char *TAG = "efuse";

#ifdef CONFIG_EFUSE_VIRTUAL
extern uint32_t virt_blocks[EFUSE_BLK_MAX][COUNT_EFUSE_REG_PER_BLOCK];
#endif // CONFIG_EFUSE_VIRTUAL

/*Range addresses to read blocks*/
const esp_efuse_range_addr_t range_read_addr_blocks[] = {
    {EFUSE_RD_WR_DIS_REG,       EFUSE_RD_REPEAT_DATA4_REG},      // range address of EFUSE_BLK0  REPEAT
    {EFUSE_RD_MAC_SPI_SYS_0_REG, EFUSE_RD_MAC_SPI_SYS_5_REG},      // range address of EFUSE_BLK1  MAC_SPI_8M
    {EFUSE_RD_SYS_PART1_DATA0_REG,    EFUSE_RD_SYS_PART1_DATA7_REG},         // range address of EFUSE_BLK2  SYS_DATA
    {EFUSE_RD_USR_DATA0_REG,    EFUSE_RD_USR_DATA7_REG},         // range address of EFUSE_BLK3  USR_DATA
    {EFUSE_RD_KEY0_DATA0_REG,   EFUSE_RD_KEY0_DATA7_REG},        // range address of EFUSE_BLK4  KEY0
    {EFUSE_RD_KEY1_DATA0_REG,   EFUSE_RD_KEY1_DATA7_REG},        // range address of EFUSE_BLK5  KEY1
    {EFUSE_RD_KEY2_DATA0_REG,   EFUSE_RD_KEY2_DATA7_REG},        // range address of EFUSE_BLK6  KEY2
    {EFUSE_RD_KEY3_DATA0_REG,   EFUSE_RD_KEY3_DATA7_REG},        // range address of EFUSE_BLK7  KEY3
    {EFUSE_RD_KEY4_DATA0_REG,   EFUSE_RD_KEY4_DATA7_REG},        // range address of EFUSE_BLK8  KEY4
    {EFUSE_RD_KEY5_DATA0_REG,   EFUSE_RD_KEY5_DATA7_REG},        // range address of EFUSE_BLK9  KEY5
    {EFUSE_RD_SYS_PART2_DATA0_REG,   EFUSE_RD_SYS_PART2_DATA7_REG}         // range address of EFUSE_BLK10 KEY6
};

static uint32_t write_mass_blocks[EFUSE_BLK_MAX][COUNT_EFUSE_REG_PER_BLOCK] = { 0 };

/*Range addresses to write blocks (it is not real regs, it is buffer) */
const esp_efuse_range_addr_t range_write_addr_blocks[] = {
    {(uint32_t) &write_mass_blocks[EFUSE_BLK0][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK0][5]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK1][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK1][5]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK2][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK2][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK3][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK3][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK4][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK4][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK5][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK5][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK6][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK6][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK7][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK7][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK8][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK8][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK9][0],  (uint32_t) &write_mass_blocks[EFUSE_BLK9][7]},
    {(uint32_t) &write_mass_blocks[EFUSE_BLK10][0], (uint32_t) &write_mass_blocks[EFUSE_BLK10][7]},
};

// Update Efuse timing configuration
static esp_err_t esp_efuse_set_timing(void)
{
    REG_SET_FIELD(EFUSE_WR_TIM_CONF2_REG, EFUSE_PWR_OFF_NUM, 0x190);
    return ESP_OK;
}

static void efuse_read(void)
{
    esp_efuse_set_timing();
    REG_WRITE(EFUSE_CONF_REG, EFUSE_READ_OP_CODE);
    REG_WRITE(EFUSE_CMD_REG, EFUSE_READ_CMD);

    while (REG_GET_BIT(EFUSE_CMD_REG, EFUSE_READ_CMD) != 0) { }
    /*Due to a hardware error, we have to read READ_CMD again to make sure the efuse clock is normal*/
    while (REG_GET_BIT(EFUSE_CMD_REG, EFUSE_READ_CMD) != 0) { }
}

#ifndef CONFIG_EFUSE_VIRTUAL
static void efuse_program(esp_efuse_block_t block)
{
    esp_efuse_set_timing();

    REG_WRITE(EFUSE_CONF_REG, EFUSE_WRITE_OP_CODE);

    REG_WRITE(EFUSE_CMD_REG, ((block << EFUSE_BLK_NUM_S) & EFUSE_BLK_NUM_M) | EFUSE_PGM_CMD);

    while (REG_GET_BIT(EFUSE_CMD_REG, EFUSE_PGM_CMD) != 0) { };

    ets_efuse_clear_program_registers();
    efuse_read();
}

static bool efuse_hal_is_coding_error_in_block(unsigned block)
{
    if (block == 0) {
        for (unsigned i = 0; i < 5; i++) {
            if (REG_READ(EFUSE_RD_REPEAT_ERR0_REG + i * 4)) {
                return true;
            }
        }
    } else if (block <= 10) {
        // The order of error in these regs is different only for the C3 chip.
        // EFUSE_RD_RS_ERR0_REG: (hi) BLOCK7, BLOCK6, BLOCK5, BLOCK4, BLOCK3, BLOCK2, BLOCK1, ------ (low)
        // EFUSE_RD_RS_ERR1_REG:                                                      BLOCK9, BLOCK8
        // BLOCK10 is not presented in the error regs.
        uint32_t error_reg = REG_READ(EFUSE_RD_RS_ERR0_REG + (block / 8) * 4);
        unsigned offset = (block >= 8) ? block - 8 : block;
        if (((error_reg >> (4 * offset)) & 0x0F) != 0) {
            return true;
        }
    }
    return false;
}

#endif // ifndef CONFIG_EFUSE_VIRTUAL

// Efuse read operation: copies data from physical efuses to efuse read registers.
void esp_efuse_utility_clear_program_registers(void)
{
    efuse_read();
    ets_efuse_clear_program_registers();
}

// Burn values written to the efuse write registers
esp_err_t esp_efuse_utility_burn_efuses(void)
{
#ifdef CONFIG_EFUSE_VIRTUAL
    ESP_LOGW(TAG, "Virtual efuses enabled: Not really burning eFuses");
    for (int num_block = EFUSE_BLK_MAX - 1; num_block >= EFUSE_BLK0; num_block--) {
        int subblock = 0;
        for (uint32_t addr_wr_block = range_write_addr_blocks[num_block].start; addr_wr_block <= range_write_addr_blocks[num_block].end; addr_wr_block += 4) {
            virt_blocks[num_block][subblock++] |= REG_READ(addr_wr_block);
        }
    }
#else
    if (esp_efuse_set_timing() != ESP_OK) {
        ESP_LOGE(TAG, "Efuse fields are not burnt");
    } else {
        // Permanently update values written to the efuse write registers
        // It is necessary to process blocks in the order from MAX-> EFUSE_BLK0, because EFUSE_BLK0 has protection bits for other blocks.
        for (int num_block = EFUSE_BLK_MAX - 1; num_block >= EFUSE_BLK0; num_block--) {
            for (uint32_t addr_wr_block = range_write_addr_blocks[num_block].start; addr_wr_block <= range_write_addr_blocks[num_block].end; addr_wr_block += 4) {
                if (REG_READ(addr_wr_block) != 0) {
                    ets_efuse_clear_program_registers();
                    if (esp_efuse_get_coding_scheme(num_block) == EFUSE_CODING_SCHEME_RS) {
                        uint8_t block_rs[12];
                        ets_efuse_rs_calculate((void *)range_write_addr_blocks[num_block].start, block_rs);
                        memcpy((void *)EFUSE_PGM_CHECK_VALUE0_REG, block_rs, sizeof(block_rs));
                    }
                    unsigned r_data_len = (range_read_addr_blocks[num_block].end - range_read_addr_blocks[num_block].start) + sizeof(uint32_t);
                    unsigned data_len = (range_write_addr_blocks[num_block].end - range_write_addr_blocks[num_block].start) + sizeof(uint32_t);
                    memcpy((void *)EFUSE_PGM_DATA0_REG, (void *)range_write_addr_blocks[num_block].start, data_len);

                    uint32_t backup_write_data[8 + 3]; // 8 words are data and 3 words are RS coding data
                    memcpy(backup_write_data, (void *)EFUSE_PGM_DATA0_REG, sizeof(backup_write_data));
                    int repeat_burn_op = 1;
                    bool correct_written_data;
                    bool coding_error_before = efuse_hal_is_coding_error_in_block(num_block);
                    bool coding_error_occurred;

                    do {
                        ESP_LOGI(TAG, "BURN BLOCK%d", num_block);
                        efuse_program(num_block); // BURN a block

                        bool coding_error_after;
                        for (unsigned i = 0; i < 5; i++) {
                            efuse_read();
                            coding_error_after = efuse_hal_is_coding_error_in_block(num_block);
                            if (coding_error_after == true) {
                                break;
                            }
                        }
                        coding_error_occurred = (coding_error_before != coding_error_after) && coding_error_before == false;
                        if (coding_error_occurred) {
                            ESP_LOGE(TAG, "BLOCK%d has an error", num_block);
                        }

                        correct_written_data = esp_efuse_utility_is_correct_written_data(num_block, r_data_len);
                        if (!correct_written_data || coding_error_occurred) {
                            ESP_LOGW(TAG, "BLOCK%d: next retry [%d/3]...", num_block, repeat_burn_op);
                            memcpy((void *)EFUSE_PGM_DATA0_REG, (void *)backup_write_data, sizeof(backup_write_data));
                        }

                    } while ((!correct_written_data || coding_error_occurred) && repeat_burn_op++ < 3);
                    if (coding_error_occurred) {
                        ESP_LOGE(TAG, "Coding error occurred in block");
                    }
                    if (!correct_written_data) {
                        ESP_LOGE(TAG, "Written data are incorrect");
                        return ESP_FAIL;
                    }
                    break;
                }
            }
        }
    }
#endif // CONFIG_EFUSE_VIRTUAL
    esp_efuse_utility_reset();
    return ESP_OK;
}

// After esp_efuse_write.. functions EFUSE_BLKx_WDATAx_REG were filled is not coded values.
// This function reads EFUSE_BLKx_WDATAx_REG registers, and checks possible to write these data with RS coding scheme.
// The RS coding scheme does not require data changes for the encoded data. esp32s2 has special registers for this.
// They will be filled during the burn operation.
esp_err_t esp_efuse_utility_apply_new_coding_scheme()
{
    // start with EFUSE_BLK1. EFUSE_BLK0 - always uses EFUSE_CODING_SCHEME_NONE.
    for (int num_block = EFUSE_BLK1; num_block < EFUSE_BLK_MAX; num_block++) {
        if (esp_efuse_get_coding_scheme(num_block) == EFUSE_CODING_SCHEME_RS) {
            for (uint32_t addr_wr_block = range_write_addr_blocks[num_block].start; addr_wr_block <= range_write_addr_blocks[num_block].end; addr_wr_block += 4) {
                if (REG_READ(addr_wr_block)) {
                    int num_reg = 0;
                    for (uint32_t addr_rd_block = range_read_addr_blocks[num_block].start; addr_rd_block <= range_read_addr_blocks[num_block].end; addr_rd_block += 4, ++num_reg) {
                        if (esp_efuse_utility_read_reg(num_block, num_reg)) {
                            ESP_LOGE(TAG, "Bits are not empty. Write operation is forbidden.");
                            return ESP_ERR_CODING;
                        }
                    }
                    break;
                }
            }
        }
    }
    return ESP_OK;
}
