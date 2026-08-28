#define _POSIX_C_SOURCE 200809L

#include "tiny_png.h"

#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static void tiny_png_error(char *buffer, size_t buffer_size,
                           const char *message)
{
    if (buffer != NULL && buffer_size > 0U) {
        (void)snprintf(buffer, buffer_size, "%s", message);
    }
}

static uint32_t tiny_png_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static bool tiny_png_add_size(size_t left, size_t right, size_t *result)
{
    if (right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static uint32_t tiny_png_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffU;
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned int bit;

        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U :
                                      crc >> 1U;
        }
    }
    return ~crc;
}

static uint16_t tiny_png_reverse_bits(uint16_t value, unsigned int count)
{
    uint16_t result = 0U;
    unsigned int index;

    for (index = 0U; index < count; ++index) {
        result = (uint16_t)((result << 1U) | (value & 1U));
        value >>= 1U;
    }
    return result;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint32_t bits;
    unsigned int bit_count;
} TinyPngBitReader;

typedef struct {
    uint16_t code[288];
    uint8_t length[288];
    uint16_t count;
} TinyPngHuffman;

static bool tiny_png_bits(TinyPngBitReader *reader, unsigned int count,
                          uint32_t *value)
{
    uint32_t mask;

    if (count > 16U || value == NULL) return false;
    while (reader->bit_count < count) {
        if (reader->position >= reader->size) return false;
        reader->bits |= (uint32_t)reader->data[reader->position++] <<
                        reader->bit_count;
        reader->bit_count += 8U;
    }
    mask = count == 0U ? 0U : (1U << count) - 1U;
    *value = reader->bits & mask;
    reader->bits >>= count;
    reader->bit_count -= count;
    return true;
}

static void tiny_png_align_byte(TinyPngBitReader *reader)
{
    reader->bits = 0U;
    reader->bit_count = 0U;
}

static bool tiny_png_huffman_build(TinyPngHuffman *tree,
                                   const uint8_t *lengths, uint16_t count)
{
    uint16_t length_count[16] = { 0 };
    uint16_t next_code[16] = { 0 };
    uint16_t code = 0U;
    uint16_t symbol;
    unsigned int bit_length;

    if (tree == NULL || lengths == NULL || count > 288U) return false;
    memset(tree, 0, sizeof(*tree));
    tree->count = count;
    for (symbol = 0U; symbol < count; ++symbol) {
        if (lengths[symbol] > 15U) return false;
        if (lengths[symbol] != 0U) length_count[lengths[symbol]]++;
    }
    for (bit_length = 1U; bit_length <= 15U; ++bit_length) {
        code = (uint16_t)((code + length_count[bit_length - 1U]) << 1U);
        next_code[bit_length] = code;
    }
    for (symbol = 0U; symbol < count; ++symbol) {
        uint8_t length = lengths[symbol];

        tree->length[symbol] = length;
        if (length != 0U) {
            tree->code[symbol] = tiny_png_reverse_bits(
                next_code[length]++, length);
        }
    }
    return true;
}

static bool tiny_png_huffman_decode(TinyPngBitReader *reader,
                                    const TinyPngHuffman *tree,
                                    uint16_t *symbol)
{
    uint16_t code = 0U;
    unsigned int length;

    for (length = 1U; length <= 15U; ++length) {
        uint32_t bit;
        uint16_t candidate;

        if (!tiny_png_bits(reader, 1U, &bit)) return false;
        code = (uint16_t)(code | (uint16_t)(bit << (length - 1U)));
        for (candidate = 0U; candidate < tree->count; ++candidate) {
            if (tree->length[candidate] == length &&
                tree->code[candidate] == code) {
                *symbol = candidate;
                return true;
            }
        }
    }
    return false;
}

static void tiny_png_fixed_trees(TinyPngHuffman *literal,
                                 TinyPngHuffman *distance)
{
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];
    unsigned int index;

    for (index = 0U; index < 288U; ++index) {
        literal_lengths[index] = index <= 143U ? 8U :
            (index <= 255U ? 9U : (index <= 279U ? 7U : 8U));
    }
    for (index = 0U; index < 32U; ++index) distance_lengths[index] = 5U;
    (void)tiny_png_huffman_build(literal, literal_lengths, 288U);
    (void)tiny_png_huffman_build(distance, distance_lengths, 32U);
}

static bool tiny_png_dynamic_trees(TinyPngBitReader *reader,
                                   TinyPngHuffman *literal,
                                   TinyPngHuffman *distance)
{
    static const uint8_t code_length_order[19] = {
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
        11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U
    };
    uint8_t code_length_lengths[19] = { 0 };
    uint8_t lengths[288 + 32];
    TinyPngHuffman code_length_tree;
    uint32_t value;
    uint32_t literal_count;
    uint32_t distance_count;
    uint32_t code_length_count;
    uint32_t index = 0U;

    if (!tiny_png_bits(reader, 5U, &value)) return false;
    literal_count = value + 257U;
    if (!tiny_png_bits(reader, 5U, &value)) return false;
    distance_count = value + 1U;
    if (!tiny_png_bits(reader, 4U, &value)) return false;
    code_length_count = value + 4U;
    if (literal_count > 286U || distance_count > 32U) return false;
    for (index = 0U; index < code_length_count; ++index) {
        if (!tiny_png_bits(reader, 3U, &value)) return false;
        code_length_lengths[code_length_order[index]] = (uint8_t)value;
    }
    if (!tiny_png_huffman_build(&code_length_tree, code_length_lengths, 19U)) {
        return false;
    }
    index = 0U;
    while (index < literal_count + distance_count) {
        uint16_t symbol;

        if (!tiny_png_huffman_decode(reader, &code_length_tree, &symbol)) {
            return false;
        }
        if (symbol <= 15U) {
            lengths[index++] = (uint8_t)symbol;
        } else if (symbol == 16U) {
            uint32_t repeat;
            uint8_t previous;

            if (index == 0U || !tiny_png_bits(reader, 2U, &value)) return false;
            repeat = value + 3U;
            previous = lengths[index - 1U];
            if (repeat > literal_count + distance_count - index) return false;
            while (repeat-- > 0U) lengths[index++] = previous;
        } else if (symbol == 17U) {
            uint32_t repeat;

            if (!tiny_png_bits(reader, 3U, &value)) return false;
            repeat = value + 3U;
            if (repeat > literal_count + distance_count - index) return false;
            while (repeat-- > 0U) lengths[index++] = 0U;
        } else if (symbol == 18U) {
            uint32_t repeat;

            if (!tiny_png_bits(reader, 7U, &value)) return false;
            repeat = value + 11U;
            if (repeat > literal_count + distance_count - index) return false;
            while (repeat-- > 0U) lengths[index++] = 0U;
        } else {
            return false;
        }
    }
    if (!tiny_png_huffman_build(literal, lengths, (uint16_t)literal_count) ||
        !tiny_png_huffman_build(distance, lengths + literal_count,
                                (uint16_t)distance_count)) {
        return false;
    }
    return true;
}

static bool tiny_png_inflate(const uint8_t *data, size_t size, uint8_t *output,
                             size_t output_size, char *error_message,
                             size_t error_message_size)
{
    static const uint16_t length_base[29] = {
        3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U,
        15U, 17U, 19U, 23U, 27U, 31U, 35U, 43U, 51U, 59U,
        67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U
    };
    static const uint8_t length_extra[29] = {
        0U,0U,0U,0U,0U,0U,0U,0U,1U,1U,1U,1U,2U,2U,2U,2U,
        3U,3U,3U,3U,4U,4U,4U,4U,5U,5U,5U,5U,0U
    };
    static const uint32_t distance_base[30] = {
        1U,2U,3U,4U,5U,7U,9U,13U,17U,25U,33U,49U,65U,97U,129U,
        193U,257U,385U,513U,769U,1025U,1537U,2049U,3073U,4097U,
        6145U,8193U,12289U,16385U,24577U
    };
    static const uint8_t distance_extra[30] = {
        0U,0U,0U,0U,1U,1U,2U,2U,3U,3U,4U,4U,5U,5U,6U,6U,7U,7U,
        8U,8U,9U,9U,10U,10U,11U,11U,12U,12U,13U,13U
    };
    TinyPngBitReader reader;
    uint32_t header;
    size_t output_position = 0U;
    bool final_block = false;

    if (data == NULL || output == NULL || size < 6U) return false;
    header = ((uint32_t)data[0] << 8U) | data[1];
    if ((data[0] & 0x0fU) != 8U || (data[0] & 0x80U) != 0U ||
        (header % 31U) != 0U) {
        tiny_png_error(error_message, error_message_size,
                       "invalid zlib header");
        return false;
    }
    reader = (TinyPngBitReader){ data + 2U, size - 6U, 0U, 0U, 0U };
    while (!final_block) {
        uint32_t final_value;
        uint32_t block_type;

        if (!tiny_png_bits(&reader, 1U, &final_value) ||
            !tiny_png_bits(&reader, 2U, &block_type)) break;
        final_block = final_value != 0U;
        if (block_type == 0U) {
            uint32_t length;
            uint32_t inverse_length;
            size_t index;

            tiny_png_align_byte(&reader);
            if (!tiny_png_bits(&reader, 16U, &length) ||
                !tiny_png_bits(&reader, 16U, &inverse_length) ||
                (length ^ 0xffffU) != inverse_length ||
                length > output_size - output_position) break;
            for (index = 0U; index < length; ++index) {
                if (!tiny_png_bits(&reader, 8U, &header)) break;
                output[output_position++] = (uint8_t)header;
            }
            if (index != length) break;
        } else if (block_type == 1U || block_type == 2U) {
            TinyPngHuffman literal_tree;
            TinyPngHuffman distance_tree;
            bool trees_ready;

            if (block_type == 1U) {
                tiny_png_fixed_trees(&literal_tree, &distance_tree);
                trees_ready = true;
            } else {
                trees_ready = tiny_png_dynamic_trees(&reader, &literal_tree,
                                                     &distance_tree);
            }
            if (!trees_ready) break;
            for (;;) {
                uint16_t symbol;

                if (!tiny_png_huffman_decode(&reader, &literal_tree, &symbol)) {
                    final_block = true;
                    break;
                }
                if (symbol < 256U) {
                    if (output_position >= output_size) break;
                    output[output_position++] = (uint8_t)symbol;
                } else if (symbol == 256U) {
                    break;
                } else if (symbol >= 257U && symbol <= 285U) {
                    uint32_t length_index = symbol - 257U;
                    uint32_t length = length_base[length_index];
                    uint32_t distance_symbol;
                    uint16_t distance_symbol_code;
                    uint32_t distance;
                    uint32_t extra = 0U;
                    size_t index;

                    if (length_extra[length_index] != 0U &&
                        !tiny_png_bits(&reader, length_extra[length_index],
                                       &extra)) break;
                    length += extra;
                    if (!tiny_png_huffman_decode(&reader, &distance_tree,
                                                 &distance_symbol_code)) break;
                    distance_symbol = distance_symbol_code;
                    if (distance_symbol >= 30U) break;
                    distance = distance_base[distance_symbol];
                    extra = 0U;
                    if (distance_extra[distance_symbol] != 0U &&
                        !tiny_png_bits(&reader, distance_extra[distance_symbol],
                                       &extra)) break;
                    distance += extra;
                    if (distance > output_position ||
                        length > output_size - output_position) break;
                    for (index = 0U; index < length; ++index) {
                        output[output_position] = output[output_position -
                                                         distance];
                        output_position++;
                    }
                } else {
                    break;
                }
                if (output_position > output_size) break;
            }
        } else {
            break;
        }
        if (output_position > output_size) break;
    }
    if (!final_block || output_position != output_size) {
        (void)snprintf(error_message, error_message_size,
                       "invalid DEFLATE stream (out=%zu/%zu final=%s bitpos=%zu/%zu)",
                       output_position, output_size, final_block ? "yes" : "no",
                       reader.position, reader.size);
        return false;
    }
    {
        uint32_t adler = 1U;
        size_t index;
        uint32_t first = 1U;
        uint32_t second = 0U;

        for (index = 0U; index < output_size; ++index) {
            first = (first + output[index]) % 65521U;
            second = (second + first) % 65521U;
        }
        adler = (second << 16U) | first;
        if (adler != ((uint32_t)data[size - 4U] << 24U |
                      (uint32_t)data[size - 3U] << 16U |
                      (uint32_t)data[size - 2U] << 8U |
                      data[size - 1U])) {
            tiny_png_error(error_message, error_message_size,
                           "zlib Adler-32 mismatch");
            return false;
        }
    }
    return true;
}

static uint8_t tiny_png_paeth(uint8_t left, uint8_t above,
                              uint8_t upper_left)
{
    int predictor = (int)left + (int)above - (int)upper_left;
    int left_distance = abs(predictor - (int)left);
    int above_distance = abs(predictor - (int)above);
    int upper_left_distance = abs(predictor - (int)upper_left);

    if (left_distance <= above_distance && left_distance <= upper_left_distance) {
        return left;
    }
    return above_distance <= upper_left_distance ? above : upper_left;
}

static bool tiny_png_unfilter(uint8_t *raw, uint32_t height,
                              size_t row_bytes, char *error_message,
                              size_t error_message_size)
{
    size_t row;
    const size_t bytes_per_pixel = 4U;

    for (row = 0U; row < height; ++row) {
        uint8_t *current = raw + row * (row_bytes + 1U) + 1U;
        const uint8_t *previous = row == 0U ? NULL :
            raw + (row - 1U) * (row_bytes + 1U) + 1U;
        uint8_t filter = raw[row * (row_bytes + 1U)];
        size_t column;

        if (filter > 4U) {
            tiny_png_error(error_message, error_message_size,
                           "unsupported PNG filter");
            return false;
        }
        for (column = 0U; column < row_bytes; ++column) {
            uint8_t left = column >= bytes_per_pixel ?
                current[column - bytes_per_pixel] : 0U;
            uint8_t above = previous != NULL ? previous[column] : 0U;
            uint8_t upper_left = previous != NULL && column >= bytes_per_pixel ?
                previous[column - bytes_per_pixel] : 0U;
            uint8_t predictor;

            switch (filter) {
            case 0U:
                predictor = 0U;
                break;
            case 1U:
                predictor = left;
                break;
            case 2U:
                predictor = above;
                break;
            case 3U:
                predictor = (uint8_t)(((unsigned int)left + above) / 2U);
                break;
            case 4U:
            default:
                predictor = tiny_png_paeth(left, above, upper_left);
                break;
            }
            current[column] = (uint8_t)(current[column] + predictor);
        }
    }
    return true;
}

static bool tiny_png_read_file(const char *path, uint8_t **data,
                               size_t *data_size, char *error_message,
                               size_t error_message_size)
{
    FILE *file;
    long file_size;
    uint8_t *contents;

    file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(error_message, error_message_size, "open: %s",
                       strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_size = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0 || file_size < 33L ||
        (unsigned long)file_size > 64UL * 1024UL * 1024UL) {
        fclose(file);
        tiny_png_error(error_message, error_message_size,
                       "invalid or oversized file");
        return false;
    }
    contents = malloc((size_t)file_size);
    if (contents == NULL) {
        fclose(file);
        tiny_png_error(error_message, error_message_size, "out of memory");
        return false;
    }
    if (fread(contents, 1U, (size_t)file_size, file) != (size_t)file_size ||
        fclose(file) != 0) {
        free(contents);
        (void)snprintf(error_message, error_message_size, "read: %s",
                       strerror(errno));
        return false;
    }
    *data = contents;
    *data_size = (size_t)file_size;
    return true;
}

static bool tiny_png_append_idat(uint8_t **idat, size_t *idat_size,
                                 const uint8_t *data, size_t data_size,
                                 char *error_message, size_t error_message_size)
{
    uint8_t *new_idat;
    size_t new_size;

    if (!tiny_png_add_size(*idat_size, data_size, &new_size)) {
        tiny_png_error(error_message, error_message_size,
                       "IDAT size overflow");
        return false;
    }
    new_idat = realloc(*idat, new_size > 0U ? new_size : 1U);
    if (new_idat == NULL && new_size > 0U) {
        tiny_png_error(error_message, error_message_size, "out of memory");
        return false;
    }
    if (data_size > 0U) {
        memcpy(new_idat + *idat_size, data, data_size);
    }
    *idat = new_idat;
    *idat_size = new_size;
    return true;
}

int tiny_png_load_rgba(const char *path, TinyPngImage *image,
                       char *error_message, size_t error_message_size)
{
    static const uint8_t signature[8] = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU
    };
    uint8_t *file_data = NULL;
    uint8_t *idat = NULL;
    uint8_t *raw = NULL;
    size_t file_size = 0U;
    size_t idat_size = 0U;
    size_t position = 8U;
    size_t row_bytes;
    size_t raw_bytes;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint8_t bit_depth = 0U;
    uint8_t color_type = 0U;
    uint8_t interlace = 0U;
    bool have_header = false;
    bool have_end = false;
    int result = -1;

    if (image == NULL || path == NULL || error_message == NULL ||
        error_message_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    memset(image, 0, sizeof(*image));
    error_message[0] = '\0';
    if (!tiny_png_read_file(path, &file_data, &file_size, error_message,
                            error_message_size) ||
        memcmp(file_data, signature, sizeof(signature)) != 0) {
        if (error_message[0] == '\0') {
            tiny_png_error(error_message, error_message_size,
                           "bad PNG signature");
        }
        goto cleanup;
    }
    while (position + 12U <= file_size) {
        uint32_t chunk_size = tiny_png_be32(file_data + position);
        const uint8_t *chunk_type = file_data + position + 4U;
        const uint8_t *chunk_data = file_data + position + 8U;
        const uint8_t *chunk_crc = chunk_data + chunk_size;
        uint32_t calculated_crc;

        if ((size_t)chunk_size > file_size - position - 12U) {
            tiny_png_error(error_message, error_message_size,
                           "truncated PNG chunk");
            goto cleanup;
        }
        calculated_crc = tiny_png_crc32(chunk_type, 4U);
        /* Continue the CRC over the chunk payload. */
        {
            uint32_t payload_crc = calculated_crc ^ 0xffffffffU;
            size_t crc_index;

            for (crc_index = 0U; crc_index < chunk_size; ++crc_index) {
                unsigned int bit;

                payload_crc ^= chunk_data[crc_index];
                for (bit = 0U; bit < 8U; ++bit) {
                    payload_crc = (payload_crc & 1U) != 0U ?
                        (payload_crc >> 1U) ^ 0xedb88320U :
                        payload_crc >> 1U;
                }
            }
            calculated_crc = ~payload_crc;
        }
        if (calculated_crc != tiny_png_be32(chunk_crc)) {
            tiny_png_error(error_message, error_message_size,
                           "PNG CRC mismatch");
            goto cleanup;
        }
        if (memcmp(chunk_type, "IHDR", 4U) == 0) {
            if (chunk_size != 13U || have_header) {
                tiny_png_error(error_message, error_message_size,
                               "invalid IHDR");
                goto cleanup;
            }
            width = tiny_png_be32(chunk_data);
            height = tiny_png_be32(chunk_data + 4U);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
            interlace = chunk_data[12];
            have_header = true;
            if (width == 0U || height == 0U || bit_depth != 8U ||
                color_type != 6U || interlace != 0U) {
                tiny_png_error(error_message, error_message_size,
                               "HUD PNG must be non-interlaced RGBA8");
                goto cleanup;
            }
        } else if (memcmp(chunk_type, "IDAT", 4U) == 0) {
            if (!have_header || !tiny_png_append_idat(
                    &idat, &idat_size, chunk_data, chunk_size, error_message,
                    error_message_size)) {
                goto cleanup;
            }
        } else if (memcmp(chunk_type, "IEND", 4U) == 0) {
            if (chunk_size != 0U) {
                tiny_png_error(error_message, error_message_size,
                               "invalid IEND");
                goto cleanup;
            }
            have_end = true;
            break;
        }
        position += (size_t)chunk_size + 12U;
    }
    if (!have_header || !have_end || idat_size == 0U) {
        tiny_png_error(error_message, error_message_size,
                       "PNG is missing IHDR, IDAT or IEND");
        goto cleanup;
    }
    if ((row_bytes = (size_t)width * 4U) > SIZE_MAX - 1U ||
        (size_t)height > SIZE_MAX / (row_bytes + 1U) ||
        (raw_bytes = (row_bytes + 1U) * (size_t)height) > SIZE_MAX / 4U) {
        tiny_png_error(error_message, error_message_size,
                       "PNG dimensions overflow");
        goto cleanup;
    }
    if ((size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / 4U) {
        tiny_png_error(error_message, error_message_size,
                       "PNG buffer size overflow");
        goto cleanup;
    }
    raw = malloc(raw_bytes);
    image->rgba = malloc((size_t)width * (size_t)height * 4U);
    if (raw == NULL || image->rgba == NULL) {
        tiny_png_error(error_message, error_message_size, "out of memory");
        goto cleanup;
    }
    if (!tiny_png_inflate(idat, idat_size, raw, raw_bytes, error_message,
                          error_message_size) ||
        !tiny_png_unfilter(raw, height, row_bytes, error_message,
                           error_message_size)) {
        if (error_message[0] == '\0') {
            tiny_png_error(error_message, error_message_size,
                           "zlib PNG inflate failed");
        }
        goto cleanup;
    }
    {
        uint32_t row;

        for (row = 0U; row < height; ++row) {
            memcpy(image->rgba + (size_t)row * row_bytes,
                   raw + (size_t)row * (row_bytes + 1U) + 1U,
                   row_bytes);
        }
    }
    image->width = width;
    image->height = height;
    image->bytes = (size_t)width * (size_t)height * 4U;
    image->alpha_min = 255U;
    image->alpha_max = 0U;
    image->alpha_x_min = width;
    image->alpha_y_min = height;
    image->alpha_x_max = 0U;
    image->alpha_y_max = 0U;
    {
        size_t pixel_index;
        for (pixel_index = 0U; pixel_index < (size_t)width * height;
             ++pixel_index) {
            uint32_t x = (uint32_t)(pixel_index % width);
            uint32_t y = (uint32_t)(pixel_index / width);
            uint8_t alpha = image->rgba[pixel_index * 4U + 3U];

            if (alpha == 0U) {
                continue;
            }
            if (alpha < image->alpha_min) image->alpha_min = alpha;
            if (alpha > image->alpha_max) image->alpha_max = alpha;
            if (x < image->alpha_x_min) image->alpha_x_min = x;
            if (y < image->alpha_y_min) image->alpha_y_min = y;
            if (x > image->alpha_x_max) image->alpha_x_max = x;
            if (y > image->alpha_y_max) image->alpha_y_max = y;
        }
    }
    result = 0;

cleanup:
    free(raw);
    free(idat);
    free(file_data);
    if (result != 0) {
        tiny_png_image_close(image);
    }
    return result;
}

void tiny_png_image_close(TinyPngImage *image)
{
    if (image == NULL) {
        return;
    }
    free(image->rgba);
    memset(image, 0, sizeof(*image));
}
