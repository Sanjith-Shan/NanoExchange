#!/usr/bin/env python3
"""Generate realistic synthetic order flow for NanoExchange and write it as CSV.

The output models a stream of exchange messages. A mid price performs a random
walk in integer ticks. New limit orders are placed around the mid with a normal
price offset. Buys sit slightly below the mid and sells slightly above. Cancels
and modifies reference an order_id that was actually emitted earlier so the
replay through the engine stays self consistent.

CSV columns.
    timestamp_ns  monotonically increasing integer nanosecond timestamp
    type          one of new, cancel, modify
    side          buy or sell
    order_type    limit, market, ioc or fok. only populated for new messages
    price         integer tick price. empty for market orders and for cancel
    quantity      integer share quantity
    order_id      integer identifier

Run it directly to produce data/order_flow.csv. See the argparse help for flags.
"""

import argparse
import csv
import random
import sys


# Approximate share of each message kind. New limit orders dominate the flow.
# The rest split across new market orders, cancels and modifies.
SHARE_NEW_LIMIT = 0.60
SHARE_NEW_MARKET = 0.10
SHARE_CANCEL = 0.20
SHARE_MODIFY = 0.10

# Order type weights for the marketable share of new limit style flow. Plain
# limit orders are the common case. ioc and fok are rarer aggressive types.
LIMIT_TYPE_WEIGHTS = [("limit", 0.80), ("ioc", 0.12), ("fok", 0.08)]

MID_START = 100000        # starting mid price in ticks
MID_STEP_MAX = 3          # max absolute integer step of the mid random walk
PRICE_OFFSET_STD = 40.0   # std of the normal price offset around the mid in ticks
SIDE_SKEW = 2             # ticks the resting side is pulled toward passive pricing
QTY_MIN = 1
QTY_MAX = 500
TS_STEP_MIN = 50          # min nanosecond gap between consecutive messages
TS_STEP_MAX = 5000        # max nanosecond gap between consecutive messages


def weighted_choice(rng, pairs):
    """Return an item from a list of value, weight pairs using rng."""
    r = rng.random()
    upto = 0.0
    for value, weight in pairs:
        upto += weight
        if r <= upto:
            return value
    return pairs[-1][0]


def generate(rows, seed, symbol):
    """Yield one order flow record dict at a time.

    symbol is accepted so future variants can vary price scale per instrument.
    It is not written as a column today because the CSV schema is single symbol
    per file. Callers pick the file name to encode the symbol.
    """
    rng = random.Random(seed)

    mid = MID_START
    ts = 0
    next_order_id = 1
    live_ids = []  # order ids emitted earlier that may be referenced later

    counts = {"new_limit": 0, "new_market": 0, "cancel": 0, "modify": 0}

    for _ in range(rows):
        ts += rng.randint(TS_STEP_MIN, TS_STEP_MAX)

        # Advance the mid price by a small integer random walk step.
        mid += rng.randint(-MID_STEP_MAX, MID_STEP_MAX)
        if mid < 1:
            mid = 1

        r = rng.random()
        want_cancel_or_modify = r >= (SHARE_NEW_LIMIT + SHARE_NEW_MARKET)

        # Cancels and modifies need a prior order id. When none exists yet we
        # fall back to emitting a new order so early rows stay valid.
        if want_cancel_or_modify and not live_ids:
            want_cancel_or_modify = False
            r = rng.random() * (SHARE_NEW_LIMIT + SHARE_NEW_MARKET)

        if not want_cancel_or_modify:
            side = "buy" if rng.random() < 0.5 else "sell"
            is_market = r >= SHARE_NEW_LIMIT
            qty = rng.randint(QTY_MIN, QTY_MAX)
            oid = next_order_id
            next_order_id += 1

            if is_market:
                counts["new_market"] += 1
                yield {
                    "timestamp_ns": ts,
                    "type": "new",
                    "side": side,
                    "order_type": "market",
                    "price": "",
                    "quantity": qty,
                    "order_id": oid,
                }
            else:
                order_type = weighted_choice(rng, LIMIT_TYPE_WEIGHTS)
                offset = int(round(rng.gauss(0.0, PRICE_OFFSET_STD)))
                if side == "buy":
                    price = mid - SIDE_SKEW - abs(offset)
                else:
                    price = mid + SIDE_SKEW + abs(offset)
                if price < 1:
                    price = 1
                counts["new_limit"] += 1
                # Only resting limit style orders are worth referencing later.
                live_ids.append(oid)
                yield {
                    "timestamp_ns": ts,
                    "type": "new",
                    "side": side,
                    "order_type": order_type,
                    "price": price,
                    "quantity": qty,
                    "order_id": oid,
                }
        elif r < (SHARE_NEW_LIMIT + SHARE_NEW_MARKET + SHARE_CANCEL):
            # Cancel a previously issued order id.
            idx = rng.randrange(len(live_ids))
            oid = live_ids.pop(idx)
            counts["cancel"] += 1
            yield {
                "timestamp_ns": ts,
                "type": "cancel",
                "side": "",
                "order_type": "",
                "price": "",
                "quantity": "",
                "order_id": oid,
            }
        else:
            # Modify the price and quantity of a previously issued order id.
            oid = rng.choice(live_ids)
            offset = int(round(rng.gauss(0.0, PRICE_OFFSET_STD)))
            new_price = mid + offset
            if new_price < 1:
                new_price = 1
            new_qty = rng.randint(QTY_MIN, QTY_MAX)
            counts["modify"] += 1
            yield {
                "timestamp_ns": ts,
                "type": "modify",
                "side": "",
                "order_type": "",
                "price": new_price,
                "quantity": new_qty,
                "order_id": oid,
            }

    generate.counts = counts


def write_csv(rows, seed, symbol, out_path):
    """Write the generated flow to out_path and return the message breakdown."""
    fieldnames = [
        "timestamp_ns",
        "type",
        "side",
        "order_type",
        "price",
        "quantity",
        "order_id",
    ]
    generate.counts = {"new_limit": 0, "new_market": 0, "cancel": 0, "modify": 0}
    with open(out_path, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        written = 0
        for record in generate(rows, seed, symbol):
            writer.writerow(record)
            written += 1
    return written, generate.counts


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate synthetic order flow CSV for NanoExchange replay."
    )
    parser.add_argument("--rows", type=int, default=200000,
                        help="number of messages to generate")
    parser.add_argument("--seed", type=int, default=42,
                        help="random seed for reproducible flow")
    parser.add_argument("--symbol", default="AAPL",
                        help="instrument symbol the flow represents")
    parser.add_argument("--out", default="data/order_flow.csv",
                        help="output CSV path")
    args = parser.parse_args(argv)

    written, counts = write_csv(args.rows, args.seed, args.symbol, args.out)

    print("NanoExchange order flow generated.")
    print("rows written  {}".format(written))
    print("path          {}".format(args.out))
    print("symbol        {}".format(args.symbol))
    print("seed          {}".format(args.seed))
    print("breakdown")
    print("  new limit   {}".format(counts["new_limit"]))
    print("  new market  {}".format(counts["new_market"]))
    print("  cancel      {}".format(counts["cancel"]))
    print("  modify      {}".format(counts["modify"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
