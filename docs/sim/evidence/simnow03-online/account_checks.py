"""Validate broker exposure from WT snapshots; zero-volume rows are valid."""
import math

def position_volume(snapshot):
    total = 0.0
    for row in snapshot.get('positions', []):
        for direction in ('long', 'short'):
            item = row[direction]
            for field in ('newvol', 'prevol'):
                value = float(item[field])
                if not math.isfinite(value) or value < 0:
                    raise ValueError('Invalid broker position volume')
                total += value
    return total
