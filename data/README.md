# NanoExchange sample market data

This folder holds order flow files that replay through the matching engine. You
can generate a synthetic file or drop in real historical data that follows the
same schema.

## Generate synthetic flow

Run the generator from the repository root.

```
python3 scripts/generate_orders.py --rows 200000 --seed 42 --symbol AAPL --out data/order_flow.csv
```

The generator models a mid price random walk in integer ticks. New limit orders
are placed around the mid with a normal price offset. Buys sit slightly below the
mid and sells slightly above. Cancels and modifies reference an order id that was
emitted earlier so the replay stays self consistent. Use `--seed` for a
reproducible file and `--rows` to size it.

## CSV schema

The file is one message per row with a header. Columns follow.

- `timestamp_ns` monotonically increasing integer nanosecond timestamp.
- `type` one of new, cancel or modify.
- `side` buy or sell. Empty for cancel and modify rows.
- `order_type` limit, market, ioc or fok. Populated only for new messages.
- `price` integer tick price. Empty for market orders and for cancel rows.
- `quantity` integer share quantity. Empty for cancel rows.
- `order_id` integer identifier. Cancel and modify rows reference an id seen earlier.

Prices are integer ticks rather than floating point dollars. This keeps the
replay deterministic and matches how the engine stores prices internally.

## Using real historical data

Real feeds such as LOBSTER message files or NASDAQ ITCH samples can be dropped in
here once converted to the schema above. Map each source event to a new, cancel
or modify row, quantize prices to integer ticks and keep the timestamps
monotonically increasing. Point the engine or the replay tool at the resulting
CSV the same way you would at a generated file.
