#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

#include <stdbool.h>
#include <stdint.h>

#define WEB_ASSET_PATH_BYTES 48U

typedef enum
{
  WEB_ASSET_CONTENT_BINARY = 0,
  WEB_ASSET_CONTENT_HTML,
  WEB_ASSET_CONTENT_JAVASCRIPT,
  WEB_ASSET_CONTENT_CSS
} WebAssetContentType;

typedef struct
{
  uint32_t flash_offset;
  uint32_t compressed_size;
  uint32_t original_size;
  char etag[17];
  WebAssetContentType content_type;
  bool immutable;
} WebAsset;

bool WebAssets_Find(const char *path, WebAsset *asset);
bool WebAssets_Read(const WebAsset *asset, uint32_t offset, void *data,
                    uint32_t size);

#endif
