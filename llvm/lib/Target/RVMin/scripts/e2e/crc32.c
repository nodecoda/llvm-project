// A realistic kernel: table-free CRC-32 over a buffer.
unsigned crc32(const unsigned char *data, unsigned len) {
  unsigned crc = 0xFFFFFFFFu;
  for (unsigned i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
  }
  return ~crc;
}

static const unsigned char sample[] = "The quick brown fox jumps over the lazy dog";

unsigned crc32_sample(void) { return crc32(sample, sizeof(sample) - 1); }
