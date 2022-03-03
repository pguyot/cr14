// SPDX-License-Identifier: GPL-2.0-only
/*
 * CR14 RFID Reader Driver
 *
 * Copyright (c) 2020 Paul Guyot <pguyot@kallisys.net>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/circ_buf.h>

// ========================================================================== //
// PROTOCOL
// ========================================================================== //

// Protocol:
// A single client can open the device at a time.
//
// Simple usage consists in opening the device read-only. It is then configured
// in poll_repeat mode, and it will repeatedly turn the CR14 on and fetch the
// uids of chips. The UIDs will be written as UID messages.
// If the device is opened for reading and writing, it will be configured in
// idle mode (awaiting commands).

// ---- UID message ----
// driver => client
// 'u' <uid in little endian (8 bytes, LSB first)>
#define MESSAGE_UID_HEADER 'u'

//
// A poll once message transitions the device in poll once mode. It will poll
// repeatedly and end as soon as a chip is found. It will then write the UID
// of the found chip.

// ---- Poll once message ----
// client => driver
// 'p'
#define MESSAGE_POLL_ONCE_HEADER 'p'


// A poll repeat message transitions the device in poll repeat mode, as
// described above.

// ---- Poll repeat message ----
// client => driver
// 'P'
#define MESSAGE_POLL_REPEAT_MODE_HEADER 'P'

//
// An idle message transitions the device in idle mode.

// ---- Idle message ----
// client => driver
// 'i'
#define MESSAGE_IDLE_HEADER 'i'

// A read single block command will transition the device in read single mode.
// It will poll repeatedly and once a chip with the matching uid is found, it
// will read the proper block and write the result to the device.

// ---- Read single block messages (request and response) ----
// client => driver
// 'r' <uid in little endian (8 bytes)> <addr (1 byte)>
// driver => client
// 'r' <data in little endian (4 bytes)>
#define MESSAGE_READ_SINGLE_BLOCK_HEADER 'r'

// A write single block command will transition the dvice in write single mode.
// It will poll repeatedly and once a chip with the matching uid is found, it
// will write the proper block, read it back and write the result to the device.

// ---- Write single block messages (request and response) ----
// client => driver
// 'w' <uid in little endian (8 bytes)> <addr (1 byte)> <data in little endian (4 bytes)>
// driver => client
// 'w' <data in little endian (4 bytes)>
#define MESSAGE_WRITE_SINGLE_BLOCK_HEADER 'w'

// A read multiple blocks command will transition the device in read multiple
// mode. It will poll repeatedly and once a chip with the matching uid is found,
// it will read the proper blocks and write the result to the device.

// ---- Read multiple block messages (request and response) ----
// client => driver
// 'R' <uid in little endian (8 bytes)> <number of addresses (1 byte)> <addresses (1-255 bytes)>
// driver => client
// 'R' <number of addresses (1 byte)> <data in little endian (4 bytes)> ... <data in little endian (4 bytes)>
#define MESSAGE_READ_MULTIPLE_BLOCKS_HEADER 'R'

// A write multiple blocks command will transition the device in write multiple
// mode. It will poll repeatedly and once a chip with the matching uid is found,
// it will write the proper blocks, read them back and write the result to the
// device.

// ---- Write multiple block messages (request and response) ----
// client => driver
// 'W' <uid in little endian (8 bytes)> <number of addresses (1 byte)> <addresses (1-255 bytes)> <data in little endian (4 bytes)> ... <data in little endian (4 bytes)>
// driver => client
// 'W' <number of addresses (1 byte)> <data in little endian (4 bytes)> ... <data in little endian (4 bytes)>
#define MESSAGE_WRITE_MULTIPLE_BLOCKS_HEADER 'W'

// ========================================================================== //
// Definitions and data structures
// ========================================================================== //

// Definitions

#define DRV_NAME "cr14"
#define DEVICE_NAME "rfid"

#define CRX14_PARAMETER_REGISTER 0x00
#define CRX14_IO_FRAME_REGISTER 0x01
#define CRX14_SLOT_MARKER_REGISTER 0x03

#define CARRIER_FREQ_RF_OUT_OFF 0x00
#define CARRIER_FREQ_RF_OUT_ON 0x10
#define WATCHDOG_TIMEOUT_5US 0x00
#define WATCHDOG_TIMEOUT_5MS 0x20
#define WATCHDOG_TIMEOUT_10MS 0x40
#define WATCHDOG_TIMEOUT_309MS 0x50

#define COMMAND_INITIATE_H 0x06
#define COMMAND_INITIATE_L 0x00
#define COMMAND_PCALL16_H 0x06
#define COMMAND_PCALL16_L 0x04
#define COMMAND_READ_BLOCK_H 0x08
#define COMMAND_WRITE_BLOCK_H 0x09
#define COMMAND_GET_UID 0x0B
#define COMMAND_RESET_TO_INVENTORY 0x0C
#define COMMAND_SELECT_H 0x0E
#define COMMAND_COMPLETION 0x0F

#define POLLING_TIMEOUT_SECS_DIV 2

enum cr14_mode {
    mode_idle,
    mode_poll_once,
    mode_poll_repeat,
    mode_read_single_block,
    mode_write_single_block,
    mode_read_multiple_blocks,
    mode_write_multiple_blocks
};

#define MAX_PACKET_SIZE 1285
#define CIRCULAR_BUFFER_SIZE 8192

#define IO_FRAME_REGISTER_MAX_RETRIES 200

// Data structures

struct cr14_read_single_block_command_params {
    u8 chip_uid[8];
    u8 addr;
};

struct cr14_write_single_block_command_params {
    u8 chip_uid[8];
    u8 addr;
    u8 data[4];
};

struct cr14_read_multiple_blocks_command_params {
    u8 chip_uid[8];
    u8 addresses_count;
    u8 addr[255];
};

struct cr14_write_multiple_blocks_command_params {
    u8 chip_uid[8];
    u8 addresses_count;
    u8 addr[255];
    u8 data[1020];
};

union cr14_command_params {
    struct cr14_read_single_block_command_params read_single_block;
    struct cr14_write_single_block_command_params write_single_block;
    struct cr14_read_multiple_blocks_command_params read_multiple_blocks;
    struct cr14_write_multiple_blocks_command_params write_multiple_blocks;
};

struct cr14_i2c_data {
    struct i2c_client *i2c;
    dev_t chrdev;
    struct class *cr14_class;
    struct cdev cdev;
    struct device *device;
	struct timer_list polling_timer;
	struct work_struct polling_work;
	spinlock_t producer_lock;
	spinlock_t consumer_lock;
	wait_queue_head_t read_wq;
	wait_queue_head_t write_wq;
    int read_buffer_head;
    int read_buffer_tail;
	char read_buffer[CIRCULAR_BUFFER_SIZE];
	int write_offset;   // current offset in write buffer
	char write_buffer[MAX_PACKET_SIZE];
	spinlock_t command_lock;    // locks mode and params
    unsigned opened:1;          // whether the device is opened
	unsigned running_command:1; // whether we're currently running a command
	enum cr14_mode mode;
	union cr14_command_params command_params;
};

// Prototypes

static void cr14_polling_timer_cb(struct timer_list *t);
static void restart_polling_timer(struct cr14_i2c_data *priv);

static int cr14_open(struct inode *inode, struct file *file);
static int cr14_release(struct inode *inode, struct file *file);
static ssize_t cr14_read(struct file *file, char __user *buffer, size_t len, loff_t *offset);
static ssize_t cr14_write(struct file *file, const char __user *buffer, size_t len, loff_t *ppos);
static unsigned int cr14_poll(struct file *file, poll_table *wait);

static int cr14_i2c_probe(struct i2c_client *i2c, const struct i2c_device_id *id);
static int cr14_i2c_remove(struct i2c_client *client);

// ========================================================================== //
// Polling code
// ========================================================================== //

static s32 cr14_write_register_byte_check(struct i2c_client *i2c, u8 reg, u8 value) {
    s32 result;
    do {
        result = i2c_smbus_write_byte_data(i2c, reg, value);
        if (result < 0) {
            break;
        }

        result = i2c_smbus_read_byte_data(i2c, reg);
        if (result < 0) {
            break;
        }

        if (result != value) {
            result = -1;
        } else {
            result = 0;
        }
    } while (0);
    return result;
}

static int cr14_read_io_frame_register(struct i2c_client *i2c, int len, u8* buffer) {
    s32 result;
    int retries = 0;
    do {
        result = i2c_smbus_read_i2c_block_data(i2c, CRX14_IO_FRAME_REGISTER, len, buffer);
        if (result == -EREMOTEIO || result == -ETIMEDOUT) {
            retries++;
        } else if (result != len) {
            dev_err(&i2c->dev, "Reading frame register failed (requested %d bytes, got %d)", len, result);
            result = -1;
        }
    } while ((result == -EREMOTEIO || result == -ETIMEDOUT) && retries < IO_FRAME_REGISTER_MAX_RETRIES);
    return result;
}

static void cr14_write_to_device(struct cr14_i2c_data *priv, int count, u8* data) {
    int ix;
    spin_lock(&priv->producer_lock);
    for (ix = 0; ix < count; ix++) {
        unsigned long head = priv->read_buffer_head;
        /* The spin_unlock() and next spin_lock() provide needed ordering. */
        unsigned long tail = READ_ONCE(priv->read_buffer_tail);
        if (CIRC_SPACE(head, tail, CIRCULAR_BUFFER_SIZE) >= count - ix) {
            priv->read_buffer[head] = data[ix];
            smp_store_release(&priv->read_buffer_head, (head + 1) & (CIRCULAR_BUFFER_SIZE - 1));
        } else {
            dev_err(&priv->i2c->dev, "Not writing to device as circular buffer would overflow");
            break;
        }
    }
    wake_up_interruptible(&priv->read_wq);
    spin_unlock(&priv->producer_lock);
}

static void cr14_process_polling(struct cr14_i2c_data *priv, const u8 *uid) {
    u8 buffer[9];
    buffer[0] = MESSAGE_UID_HEADER;
    memcpy(buffer + 1, uid, sizeof(buffer) - 1);
    cr14_write_to_device(priv, sizeof(buffer), buffer);
    
    if (priv->mode == mode_poll_once) {
        priv->mode = mode_idle;
    }
}

static int cr14_write_block(struct i2c_client *i2c, u8 addr, const u8 *data) {
    s32 result;
    u8 buffer[7];
    buffer[0] = 6;
    buffer[1] = COMMAND_WRITE_BLOCK_H;
    buffer[2] = addr;
    buffer[3] = data[0];
    buffer[4] = data[1];
    buffer[5] = data[2];
    buffer[6] = data[3];
    result = i2c_smbus_write_i2c_block_data(i2c, CRX14_IO_FRAME_REGISTER, 7, buffer);
    if (result < 0) {
        dev_err(&i2c->dev, "Writing frame register failed (%d)", result);
    } else {
        // 6 bytes + 7ms worst case (binary counter decrement)
        usleep_range(8650, 10000);
    }
    return result;
}

// Return 0 on success.
// 1 on collision
// 2 if chip disappeared
// negative value on error.
static int cr14_read_block(struct i2c_client *i2c, u8 addr, u8 *data) {
    s32 result;
    u8 buffer[5];
    buffer[0] = 2;
    buffer[1] = COMMAND_READ_BLOCK_H;
    buffer[2] = addr;
    result = i2c_smbus_write_i2c_block_data(i2c, CRX14_IO_FRAME_REGISTER, 3, buffer);
    if (result < 0) {
        dev_err(&i2c->dev, "Writing frame register failed (%d)", result);
    } else {
        // 2 bytes, see below
        usleep_range(1250, 2000);
        result = cr14_read_io_frame_register(i2c, 5, buffer);
        if (result < 0) {
            dev_err(&i2c->dev, "Reading frame register failed (%d)", result);
        } else if (buffer[0] == 255) {
            // CRC mismatch, reset to inventory for next anti-collision sequence.
            buffer[0] = 1;
            buffer[1] = COMMAND_RESET_TO_INVENTORY;
            result = i2c_smbus_write_i2c_block_data(i2c, CRX14_IO_FRAME_REGISTER, 2, buffer);
            if (result < 0) {
                dev_err(&i2c->dev, "Writing frame register failed (%d)", result);
            }
            // 1 byte: 651 usec + watchdog-timeout.
            usleep_range(1200, 2000);
            result = 1;
        } else if (buffer[0] == 0) {
            // Chip did not reply, leave.
            result = 2;
        } else if (buffer[0] != 4) {
            // Incoherent number of bytes
            if (result < 0) {
                dev_err(&i2c->dev, "Expected 4 bytes for read_block, got %d instead", buffer[0]);
            }
        } else {
            data[0] = buffer[1];
            data[1] = buffer[2];
            data[2] = buffer[3];
            data[3] = buffer[4];
            result = 0;
        }
    }
    return result;
}

static int cr14_process_command(struct cr14_i2c_data *priv) {
    s32 result;
    int ix;
    int collision = 0;
    do {
        if (priv->mode == mode_write_single_block) {
            result = cr14_write_block(
                priv->i2c,
                priv->command_params.write_single_block.addr,
                priv->command_params.write_single_block.data);
            if (result < 0) {
                break;
            }
        } else if (priv->mode == mode_write_multiple_blocks) {
            for (ix = 0; ix < priv->command_params.write_multiple_blocks.addresses_count; ix++) {
                u8 addr = priv->command_params.write_multiple_blocks.addr[ix];
                u8* data = priv->command_params.write_multiple_blocks.data + (ix * 4);
                result = cr14_write_block(priv->i2c, addr, data);
                if (result < 0) {
                    break;
                }
            }
            if (result < 0) {
                break;
            }
        }
        if (priv->mode == mode_read_single_block || priv->mode == mode_write_single_block) {
            u8 addr;
            u8 buffer[5];
            if (priv->mode == mode_read_single_block) {
                addr = priv->command_params.read_single_block.addr;
            } else {
                addr = priv->command_params.write_single_block.addr;
            }
            result = cr14_read_block(priv->i2c, addr, buffer + 1);
            if (result) {
                if (result == 1) {
                    collision = 1;
                }
                break;
            }
            if (priv->mode == mode_read_single_block) {
                buffer[0] = MESSAGE_READ_SINGLE_BLOCK_HEADER;
            } else {
                buffer[0] = MESSAGE_WRITE_SINGLE_BLOCK_HEADER;
            }
            cr14_write_to_device(priv, 5, buffer);
            priv->mode = mode_idle;
        } else {
            u8 *read_data;
            u8 *addresses;
            u8 addresses_count;
            if (priv->mode == mode_read_multiple_blocks) {
                addresses = priv->command_params.read_multiple_blocks.addr;
                addresses_count = priv->command_params.read_multiple_blocks.addresses_count;
            } else {
                addresses = priv->command_params.write_multiple_blocks.addr;
                addresses_count = priv->command_params.write_multiple_blocks.addresses_count;
            }
            read_data = devm_kzalloc(&priv->i2c->dev, 2 + (addresses_count * 4), GFP_KERNEL);
            if (!read_data) {
                result = -ENOMEM;
                break;
            }
            result = 0;
            for (ix = 0; ix < addresses_count; ix++) {
                result = cr14_read_block(priv->i2c, addresses[ix], read_data + 2 + (4*ix));
                if (result) {
                    if (result == 1) {
                        collision = 1;
                    }
                    break;
                }
            }
            if (result) {
                devm_kfree(&priv->i2c->dev, read_data);
                break;
            }
            if (priv->mode == mode_read_multiple_blocks) {
                read_data[0] = MESSAGE_READ_MULTIPLE_BLOCKS_HEADER;
            } else {
                read_data[0] = MESSAGE_WRITE_MULTIPLE_BLOCKS_HEADER;
            }
            read_data[1] = addresses_count;
            cr14_write_to_device(priv, 2 + (addresses_count * 4), read_data);
            priv->mode = mode_idle;
            devm_kfree(&priv->i2c->dev, read_data);
        }
    } while (false);
    return collision;
}

static int cr14_get_uid_and_process_mode(struct cr14_i2c_data *priv, u8 chip_id) {
    u8 buffer[9];
    s32 result;
    int collision = 0;
    do {
        buffer[0] = 2;
        buffer[1] = COMMAND_SELECT_H;
        buffer[2] = chip_id;
        result = i2c_smbus_write_i2c_block_data(priv->i2c, CRX14_IO_FRAME_REGISTER, 3, buffer);
        if (result < 0) {
            dev_err(&priv->i2c->dev, "Writing frame register failed (%d)", result);
            break;
        }
        // 2 bytes, see below
        usleep_range(1250, 2000);
        result = cr14_read_io_frame_register(priv->i2c, 2, buffer);
        if (result < 0) {
            dev_err(&priv->i2c->dev, "Reading frame register failed (%d)", result);
            break;
        } else if (buffer[0] == 255) {
            // CRC mismatch, reset to inventory for next anti-collision sequence.
            buffer[0] = 1;
            buffer[1] = COMMAND_RESET_TO_INVENTORY;
            result = i2c_smbus_write_i2c_block_data(priv->i2c, CRX14_IO_FRAME_REGISTER, 2, buffer);
            if (result < 0) {
                dev_err(&priv->i2c->dev, "Writing frame register failed (%d)", result);
            }
            // 1 byte: 651 usec + watchdog-timeout.
            usleep_range(1200, 2000);
            collision = 1;
            break;
        } else if (buffer[0] == 0) {
            // Chip did not reply, leave.
            break;
        } else if (buffer[0] != 1) {
            // CR14 didn't send the len as first byte
            dev_err(&priv->i2c->dev, "Select did not return a single byte, first byte is %d", buffer[0]);
            break;
        } else if (buffer[1] != chip_id) {
            dev_err(&priv->i2c->dev, "Select did not return the chip_id (%d, chip_id = %d)", buffer[1], chip_id);
            break;
        } else {
            // Select succeeded.
            buffer[0] = 1;
            buffer[1] = COMMAND_GET_UID;
            result = i2c_smbus_write_i2c_block_data(priv->i2c, CRX14_IO_FRAME_REGISTER, 2, buffer);
            if (result < 0) {
                dev_err(&priv->i2c->dev, "Writing frame register failed (%d)", result);
                break;
            }
            // We expect the PICC to write the result, which is 8 bytes (+ CRC)
            // Default case is therefore longer than timeout.
            // 10 bytes => 100 ETU
            // SOF & EOF => 26 ETU
            usleep_range(1900, 5000);

            result = cr14_read_io_frame_register(priv->i2c, 9, buffer);
            if (result < 0) {
                dev_err(&priv->i2c->dev, "Reading frame register failed (%d)", result);
                break;
            }
            if (buffer[0] == 255) {
                // CRC mismatch, reset to inventory for next anti-collision sequence, if any.
                buffer[0] = 1;
                buffer[1] = COMMAND_RESET_TO_INVENTORY;
                result = i2c_smbus_write_i2c_block_data(priv->i2c, CRX14_IO_FRAME_REGISTER, 2, buffer);
                if (result < 0) {
                    dev_err(&priv->i2c->dev, "Writing frame register failed (%d)", result);
                }
                // 1 byte: 651 usec + watchdog-timeout.
                usleep_range(1200, 2000);
                collision = 1;
                break;
            }
            if (buffer[0] != 8) {
                dev_err(&priv->i2c->dev, "UID length mismatch, expected 8 bytes, first byte is %d", buffer[0]);
                break;
            }
            
            // Process UID depending on mode.
            if (priv->mode == mode_poll_once || priv->mode == mode_poll_repeat) {
                cr14_process_polling(priv, buffer + 1);
            } else if (priv->mode != mode_idle) {
                // Check if UID matches.
                const u8* chip_uid;
                switch (priv->mode) {
                    case mode_read_single_block:
                        chip_uid = priv->command_params.read_single_block.chip_uid;
                        break;
                    case mode_read_multiple_blocks:
                        chip_uid = priv->command_params.read_multiple_blocks.chip_uid;
                        break;
                    case mode_write_single_block:
                        chip_uid = priv->command_params.write_single_block.chip_uid;
                        break;
                    case mode_write_multiple_blocks:
                        chip_uid = priv->command_params.write_multiple_blocks.chip_uid;
                        break;
                    default:
                        chip_uid = NULL;
                }
                if (memcmp(chip_uid, buffer + 1, 8) == 0) {
                    if (cr14_process_command(priv)) {
                        collision = 1;
                    }
                }
            } else {
                dev_err(&priv->i2c->dev, "Unknown mode (%d)", priv->mode);
            }

            // Send completion command: chip will no longer participate in
            // anti-collision protocol
            buffer[0] = 1;
            buffer[1] = COMMAND_COMPLETION;
            result = i2c_smbus_write_i2c_block_data(priv->i2c, CRX14_IO_FRAME_REGISTER, 2, buffer);
            if (result < 0) {
                dev_err(&priv->i2c->dev, "Writing frame register failed (%d)", result);
            }
            // 1 byte, see above.
            usleep_range(1200, 2000);
        }
    } while (0);
    return collision;
}

static void cr14_do_poll(struct work_struct *work) {
    struct cr14_i2c_data *priv = container_of(work, struct cr14_i2c_data, polling_work);
    // Copy mode and params to the stack.
    s32 result;
    u8 buffer[36];
    u8 value;
    int collision;

    spin_lock(&priv->command_lock);
    if (priv->mode == mode_idle) {
        spin_unlock(&priv->command_lock);
        return;
    }

    do {
        // Turn RF on.
        value = CARRIER_FREQ_RF_OUT_ON | WATCHDOG_TIMEOUT_5US;
        result = cr14_write_register_byte_check(priv->i2c, CRX14_PARAMETER_REGISTER, value);
        if (result < 0) {
            dev_err(&priv->i2c->dev, "Turning RF on failed (%d)", result);
            break;
        }
        
        buffer[0] = 2;
        buffer[1] = COMMAND_INITIATE_H;
        buffer[2] = COMMAND_INITIATE_L;
        result = i2c_smbus_write_i2c_block_data(priv->i2c, CRX14_IO_FRAME_REGISTER, 3, buffer);
        if (result < 0) {
            dev_err(&priv->i2c->dev, "Writing frame register failed (%d)", result);
            break;
        }
        // After each write to the frame register, we need to wait for the CR14
        // to send the command and wait for the result.
        // We will perform busy polling as described in the datasheet.
        // However, we know we can wait a minium time based on the number of
        // sent or expected bytes.
        // Time to send two bytes is 745 usec (61 ETU + t0 + t1 wait times)
        // Watch-dog timeout is 500 usec.
        // => wait at least 1250 usec.
        usleep_range(1250, 2000);

        result = cr14_read_io_frame_register(priv->i2c, 2, buffer);
        if (result < 0) {
            dev_err(&priv->i2c->dev, "Reading frame register failed (%d)", result);
            break;
        }
        if (buffer[0] == 255) {
            collision = 1;
        } else {
            collision = 0;
        }

        priv->running_command = 1;  // lock mode & params
        do {
            if (collision) {
                u16 mask;
                int ix;

                collision = 0;
                result = i2c_smbus_write_byte(priv->i2c, CRX14_SLOT_MARKER_REGISTER);
                if (result < 0) {
                    dev_err(&priv->i2c->dev, "Writing slot marker register failed (%d)", result);
                    break;
                }
                // Wait much longer here:
                // 49 bytes => 490 ETU
                // 16 SOF & 16 EOF => 336 ETU
                // 16 watch-dog timeouts => 8000 usec
                // at least 16000 usecs
                usleep_range(16000, 20000);
            
                result = cr14_read_io_frame_register(priv->i2c, 19, buffer);
                if (result < 0) {
                    dev_err(&priv->i2c->dev, "Reading frame register failed (%d)", result);
                    break;
                }
                if (buffer[0] != 18) {
                    dev_err(&priv->i2c->dev, "Slot marker did not return 18 bytes, first byte is %d", buffer[0]);
                    break;
                }
                mask = (buffer[2] << 8) | buffer[1];
                ix = 0;
                for (ix = 0; ix < 16; ix++) {
                    if (mask & 0x0001) {
                        u8 chip_id = buffer[ix + 3];
                        if (cr14_get_uid_and_process_mode(priv, chip_id)) {
                            collision = 1;
                        }
                    } else if (buffer[ix + 3] == 0xFF) {
                        collision = 1;
                    }
                    mask >>= 1;
                }
            } else {
                // A single PICC returned its id.
                if (cr14_get_uid_and_process_mode(priv, buffer[1])) {
                    // In case of CRC error, retry with the slot marker route
                    collision = 1;
                }
            }
        } while (collision != 0);
    } while (0);

    priv->running_command = 0;  // unlock mode & params
    wake_up_interruptible(&priv->write_wq);
    spin_unlock(&priv->command_lock);

    value = CARRIER_FREQ_RF_OUT_OFF | WATCHDOG_TIMEOUT_5US;
    result = i2c_smbus_write_byte_data(priv->i2c, CRX14_PARAMETER_REGISTER, value);
    if (result < 0) {
        dev_err(&priv->i2c->dev, "Turning RF off failed (%d)", result);
    }

    if (priv->mode != mode_idle) {
        restart_polling_timer(priv);
    }
}

static void cr14_polling_timer_cb(struct timer_list *t) {
    struct cr14_i2c_data *priv = from_timer(priv, t, polling_timer);
    if (priv->opened) {
    	schedule_work(&priv->polling_work);
    }
}

static void restart_polling_timer(struct cr14_i2c_data *priv) {
    del_timer_sync(&priv->polling_timer);
    mod_timer(&priv->polling_timer, jiffies + HZ / POLLING_TIMEOUT_SECS_DIV);
}

static void stop_polling_timer(struct cr14_i2c_data *priv) {
    del_timer_sync(&priv->polling_timer);
}

static void trigger_polling_work(struct cr14_i2c_data *priv) {
    del_timer_sync(&priv->polling_timer);
    if (priv->opened) {
    	schedule_work(&priv->polling_work);
    }
}

// ========================================================================== //
// File operations & commands
// ========================================================================== //

static int cr14_open(struct inode *inode, struct file *file) {
    struct cr14_i2c_data *priv;
    priv = container_of(inode->i_cdev, struct cr14_i2c_data, cdev);
    file->private_data = priv;

    if (priv->opened) {
        return -EBUSY;
    }
    priv->opened = 1;
    priv->running_command = 0;
    priv->write_offset = 0;
    priv->read_buffer_head = 0;
    priv->read_buffer_tail = 0;
    if (file->f_mode & FMODE_WRITE) {
        priv->mode = mode_idle;
    } else {
        priv->mode = mode_poll_repeat;
    }
    schedule_work(&priv->polling_work);
    
    return 0;
}

static int cr14_release(struct inode *inode, struct file *file) {
    struct cr14_i2c_data *priv;
    priv = container_of(inode->i_cdev, struct cr14_i2c_data, cdev);
    priv->opened = 0;
    
    cancel_work_sync(&priv->polling_work);
    stop_polling_timer(priv);

    return 0;
}

static ssize_t cr14_read(struct file *file, char __user *buffer, size_t len, loff_t *ppos) {
    struct cr14_i2c_data *priv = (struct cr14_i2c_data *) file->private_data;
    int read_count = 0;
    spin_lock(&priv->consumer_lock);
    if (wait_event_interruptible(priv->read_wq, priv->read_buffer_head != priv->read_buffer_tail)) {
        spin_unlock(&priv->consumer_lock);
        return -ERESTARTSYS;
    }
    /* Read index before reading contents at that index. */
    while (len > 0) {
        unsigned long head = smp_load_acquire(&priv->read_buffer_head);
        unsigned long tail = priv->read_buffer_tail;
        if (CIRC_CNT(head, tail, CIRCULAR_BUFFER_SIZE) >= 1) {
            if (copy_to_user(buffer, &priv->read_buffer[tail], 1)) {
                read_count = -EFAULT;
                break;
            }
            buffer++;
            read_count++;
            len--;
            /* Finish reading descriptor before incrementing tail. */
            smp_store_release(&priv->read_buffer_tail, (tail + 1) & (CIRCULAR_BUFFER_SIZE - 1));
        } else {
            // Len was exhausted, exit
            break;
        }
    }
    spin_unlock(&priv->consumer_lock);
    if (read_count > 0) {
        *ppos += read_count;
    }
    return read_count;
}

static ssize_t cr14_write(struct file *file, const char __user *buffer, size_t len, loff_t *ppos) {
    struct cr14_i2c_data *priv = (struct cr14_i2c_data *) file->private_data;
    int written_count = 0;
    if (len <= 0) {
        return 0;
    }
    spin_lock(&priv->command_lock);
    if (wait_event_interruptible(priv->write_wq, priv->running_command == 0)) {
        spin_unlock(&priv->command_lock);
        return -ERESTARTSYS;
    }
    do {
        int packet_len = 0;
        char mode_header;
        if (priv->write_offset == 0) {
            // Next byte is message header.
            if (copy_from_user(priv->write_buffer, buffer, 1)) {
                written_count = -EFAULT;
                break;
            }
            len--;
            written_count++;
            mode_header = priv->write_buffer[0];
            if (mode_header == MESSAGE_IDLE_HEADER) {
                priv->mode = mode_idle;
                break;
            } else if (mode_header == MESSAGE_POLL_ONCE_HEADER) {
                priv->mode = mode_poll_once;
                trigger_polling_work(priv);
                break;
            } else if (mode_header == MESSAGE_POLL_REPEAT_MODE_HEADER) {
                priv->mode = mode_poll_repeat;
                trigger_polling_work(priv);
                break;
            }
            priv->write_offset++;
            buffer++;
        } else {
            mode_header = priv->write_buffer[0];
        }
        if (mode_header == MESSAGE_READ_SINGLE_BLOCK_HEADER) {
            packet_len = 10;
        } else if (mode_header == MESSAGE_WRITE_SINGLE_BLOCK_HEADER) {
            packet_len = 14;
        } else if (mode_header == MESSAGE_READ_MULTIPLE_BLOCKS_HEADER) {
            packet_len = 10;
        } else if (mode_header == MESSAGE_WRITE_MULTIPLE_BLOCKS_HEADER) {
            packet_len = 10;
        }
        if (priv->write_offset < packet_len) {
            int attempt_count = packet_len - priv->write_offset;
            if (attempt_count > len) {
                attempt_count = len;
            }
            if (copy_from_user(priv->write_buffer + priv->write_offset, buffer, attempt_count)) {
                written_count = -EFAULT;
                break;
            }
            len -= attempt_count;
            written_count += attempt_count;
            priv->write_offset += attempt_count;
            buffer += attempt_count;
        }
        // Fix packet_len with variable-size packets.
        if (priv->write_offset >= packet_len) {
            if (mode_header == MESSAGE_READ_MULTIPLE_BLOCKS_HEADER) {
                packet_len = 10 + (priv->write_buffer[9]);
            } else if (mode_header == MESSAGE_WRITE_MULTIPLE_BLOCKS_HEADER) {
                packet_len = 10 + (priv->write_buffer[9] * 5);
            }
        }
        // Read variable-size data
        if (priv->write_offset < packet_len) {
            int attempt_count = packet_len - priv->write_offset;
            if (attempt_count > len) {
                attempt_count = len;
            }
            if (copy_from_user(priv->write_buffer + priv->write_offset, buffer, attempt_count)) {
                written_count = -EFAULT;
                break;
            }
            len -= attempt_count;
            written_count += attempt_count;
            priv->write_offset += attempt_count;
            buffer += attempt_count;
        }
        if (priv->write_offset == packet_len) {
            // End of packet.
            int addr_count;
            switch (priv->write_buffer[0]) {
                case MESSAGE_READ_SINGLE_BLOCK_HEADER:
                    priv->mode = mode_read_single_block;
                    memcpy(priv->command_params.read_single_block.chip_uid, priv->write_buffer + 1, 8);
                    priv->command_params.read_single_block.addr = priv->write_buffer[9];
                    break;

                case MESSAGE_WRITE_SINGLE_BLOCK_HEADER:
                    priv->mode = mode_write_single_block;
                    memcpy(priv->command_params.write_single_block.chip_uid, priv->write_buffer + 1, 8);
                    priv->command_params.write_single_block.addr = priv->write_buffer[9];
                    memcpy(priv->command_params.write_single_block.data, priv->write_buffer + 10, 4);
                    break;

                case MESSAGE_READ_MULTIPLE_BLOCKS_HEADER:
                    priv->mode = mode_read_multiple_blocks;
                    memcpy(priv->command_params.read_multiple_blocks.chip_uid, priv->write_buffer + 1, 8);
                    addr_count = priv->write_buffer[9];
                    priv->command_params.read_multiple_blocks.addresses_count = addr_count;
                    memcpy(priv->command_params.read_multiple_blocks.addr, priv->write_buffer + 10, addr_count);
                    break;

                case MESSAGE_WRITE_MULTIPLE_BLOCKS_HEADER:
                    priv->mode = mode_write_multiple_blocks;
                    memcpy(priv->command_params.write_multiple_blocks.chip_uid, priv->write_buffer + 1, 8);
                    addr_count = priv->write_buffer[9];
                    priv->command_params.write_multiple_blocks.addresses_count = addr_count;
                    memcpy(priv->command_params.write_multiple_blocks.addr, priv->write_buffer + 10, addr_count);
                    memcpy(priv->command_params.write_multiple_blocks.data, priv->write_buffer + 10 + addr_count, addr_count * 4);
                    break;
            }
            priv->write_offset = 0;
            trigger_polling_work(priv);
        }
    } while (0);
    spin_unlock(&priv->command_lock);
    if (written_count > 0) {
        *ppos += written_count;
    }
    return written_count;
}

static unsigned int cr14_poll(struct file *file, poll_table *wait) {
    struct cr14_i2c_data *priv = (struct cr14_i2c_data *) file->private_data;
    unsigned int mask = 0;

    poll_wait(file, &priv->read_wq, wait);
    poll_wait(file, &priv->write_wq, wait);
    spin_lock(&priv->consumer_lock);
    if (priv->read_buffer_head != priv->read_buffer_tail) {
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock(&priv->consumer_lock);
    if (priv->running_command == 0) {
        mask |= POLLOUT | POLLWRNORM;
    }

    return mask;
}

static struct file_operations cr14_fops = {
    .owner = THIS_MODULE,
    .open = cr14_open,
    .read = cr14_read,
    .write = cr14_write,
    .release = cr14_release,
    .poll = cr14_poll,
};

// ========================================================================== //
// Probing, initialization and cleanup
// ========================================================================== //

static int cr14_i2c_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
	struct cr14_i2c_data *priv;
    struct device *dev = &i2c->dev;
    int err;
    s32 result;

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    // Read parameter register to make sure device is connected.
    result = i2c_smbus_read_byte_data(i2c, CRX14_PARAMETER_REGISTER);
    if (result < 0) {
        dev_err(dev, "Could not read parameter register %d", result);
        return result;
    }
    
    i2c_set_clientdata(i2c, priv);
    priv->i2c = i2c;

    timer_setup(&priv->polling_timer, cr14_polling_timer_cb, 0);
    
    // Register device.
    err = alloc_chrdev_region(&priv->chrdev, 0, 2, DEVICE_NAME);
    if (err < 0) {
        dev_err(dev, "Failed to registering character device: %d", err);
        cr14_i2c_remove(i2c);
        return err;
    }

	// Create device class
	priv->cr14_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(priv->cr14_class)) {
		err = PTR_ERR(priv->cr14_class);
        dev_err(dev, "class_create failed: %d", err);
        cr14_i2c_remove(i2c);
        return err;
	}

    cdev_init(&priv->cdev, &cr14_fops);

    err = cdev_add(&priv->cdev, priv->chrdev, 1);
    if (err) {
        dev_err(dev, "Failed to add cdev: %d", err);
        cr14_i2c_remove(i2c);
        return err;
    }

	priv->device = device_create(priv->cr14_class, dev, priv->chrdev, NULL, /* no additional data */ DEVICE_NAME "%d", MINOR(priv->chrdev));
	if (IS_ERR(priv->device)) {
		err = PTR_ERR(priv->device);
        dev_err(dev, "Failed to create device: %d", err);
        cr14_i2c_remove(i2c);
        return err;
    }

    spin_lock_init(&priv->producer_lock);
    spin_lock_init(&priv->consumer_lock);
    spin_lock_init(&priv->command_lock);
    init_waitqueue_head(&priv->read_wq);
    init_waitqueue_head(&priv->write_wq);
	INIT_WORK(&priv->polling_work, cr14_do_poll);

    return 0;
}

static int cr14_i2c_remove(struct i2c_client *client)
{
    struct cr14_i2c_data *priv;
    priv = i2c_get_clientdata(client);

    if (priv->chrdev) {
        if (priv->cr14_class) {
            if (priv->cdev.ops) {
                device_destroy(priv->cr14_class, MKDEV(MAJOR(priv->chrdev), MINOR(priv->chrdev)));
                cdev_del(&priv->cdev);
            }
            class_destroy(priv->cr14_class);
        }
        unregister_chrdev_region(priv->chrdev, 2);
    }

    del_timer_sync(&priv->polling_timer);
	cancel_work_sync(&priv->polling_work);
    
    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id cr14_i2c_ids[] = {
    { .compatible = "stm,cr14", },
    { }
};
MODULE_DEVICE_TABLE(of, cr14_i2c_ids);
#endif

static struct i2c_driver cr14_i2c_driver = {
    .driver = {
        .name = DRV_NAME,
        .of_match_table = of_match_ptr(cr14_i2c_ids),
    },
    .probe              = cr14_i2c_probe,
    .remove             = cr14_i2c_remove,
};

module_i2c_driver(cr14_i2c_driver);

MODULE_DESCRIPTION("STMicroelectronics CR14 Driver");
MODULE_AUTHOR("Paul Guyot <pguyot@kallisys.net>");
MODULE_LICENSE("GPL");
