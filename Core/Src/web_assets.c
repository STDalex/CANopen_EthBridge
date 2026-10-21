#include "web_assets.h"

#include "qspi_store.h"

#include <stddef.h>
#include <string.h>

#define WEB_IMAGE_ADDRESS       0x00000000UL
#define WEB_IMAGE_MAX_BYTES     0x00100000UL
#define WEB_IMAGE_MAGIC         0x42575343UL /* "CSWB", little-endian. */
#define WEB_IMAGE_VERSION       1U
#define WEB_HEADER_BYTES        32U
#define WEB_ENTRY_BYTES         80U
#define WEB_ENTRY_MAX           8U

static uint16_t ReadLe16(const uint8_t *source)
{
  return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t ReadLe32(const uint8_t *source)
{
  return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
         ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

static uint32_t Crc32(const uint8_t *data, uint32_t size)
{
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t bit;
  while (size-- != 0U)
  {
    crc ^= *data++;
    for (bit = 0U; bit < 8U; ++bit)
    {
      crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
  }
  return ~crc;
}

bool WebAssets_Find(const char *path, WebAsset *asset)
{
  uint8_t manifest[WEB_HEADER_BYTES + (WEB_ENTRY_MAX * WEB_ENTRY_BYTES)];
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t image_size;
  uint32_t stored_crc;
  uint32_t manifest_size;
  uint32_t index;

  if ((path == NULL) || (asset == NULL) ||
      !QspiStore_Read(WEB_IMAGE_ADDRESS, manifest, WEB_HEADER_BYTES))
  {
    return false;
  }
  magic = ReadLe32(manifest);
  version = ReadLe16(manifest + 4U);
  count = ReadLe16(manifest + 6U);
  image_size = ReadLe32(manifest + 8U);
  stored_crc = ReadLe32(manifest + 12U);
  if ((magic != WEB_IMAGE_MAGIC) || (version != WEB_IMAGE_VERSION) ||
      (count == 0U) || (count > WEB_ENTRY_MAX) ||
      (image_size > WEB_IMAGE_MAX_BYTES))
  {
    return false;
  }
  manifest_size = WEB_HEADER_BYTES + ((uint32_t)count * WEB_ENTRY_BYTES);
  if (!QspiStore_Read(WEB_IMAGE_ADDRESS, manifest, manifest_size))
  {
    return false;
  }
  memset(manifest + 12U, 0, 4U);
  if (Crc32(manifest, manifest_size) != stored_crc)
  {
    return false;
  }

  for (index = 0U; index < count; ++index)
  {
    const uint8_t *entry = manifest + WEB_HEADER_BYTES + (index * WEB_ENTRY_BYTES);
    char entry_path[WEB_ASSET_PATH_BYTES];
    uint32_t offset;
    uint32_t compressed_size;
    memcpy(entry_path, entry, sizeof(entry_path));
    entry_path[sizeof(entry_path) - 1U] = '\0';
    if (strcmp(path, entry_path) != 0)
    {
      continue;
    }
    offset = ReadLe32(entry + 48U);
    compressed_size = ReadLe32(entry + 52U);
    if ((offset < manifest_size) || (compressed_size == 0U) ||
        (offset > image_size) || (compressed_size > image_size - offset))
    {
      return false;
    }
    asset->flash_offset = offset;
    asset->compressed_size = compressed_size;
    asset->original_size = ReadLe32(entry + 56U);
    memcpy(asset->etag, entry + 60U, 16U);
    asset->etag[16] = '\0';
    asset->content_type = entry[76U] <= WEB_ASSET_CONTENT_CSS ?
                          (WebAssetContentType)entry[76U] :
                          WEB_ASSET_CONTENT_BINARY;
    asset->immutable = entry[77U] != 0U;
    return true;
  }
  return false;
}

bool WebAssets_Read(const WebAsset *asset, uint32_t offset, void *data,
                    uint32_t size)
{
  if ((asset == NULL) || (data == NULL) ||
      (offset > asset->compressed_size) ||
      (size > asset->compressed_size - offset))
  {
    return false;
  }
  return QspiStore_Read(WEB_IMAGE_ADDRESS + asset->flash_offset + offset,
                        data, size);
}
