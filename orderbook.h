#ifndef __ORDERBOOK_H__
#define __ORDERBOOK_H__

#include <stdint.h>
#include <cjson/cJSON.h>

#define OB_LEVELS 200
#define CRC_OB_LEVELS 25

struct PriceLevel {
  double price;
  double quantity;
  char price_str[32]; // checksum needs original strings from fetched data
  char qty_str[32];
};

struct Orderbook {
  struct PriceLevel bids[OB_LEVELS];
  struct PriceLevel asks[OB_LEVELS];
  int count_bids;
  int count_asks;
  uint64_t last_ts;
  uint64_t seq;
  int32_t checksum;
};

void ob_apply_snapshot(struct Orderbook *ob, cJSON *data);
void ob_apply_update(struct Orderbook *ob, cJSON *data);
void add_level(struct PriceLevel *level, int *count, double price, double quantity, int bid);

#endif