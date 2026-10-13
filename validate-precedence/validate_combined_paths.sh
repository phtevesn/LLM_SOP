#!/usr/bin/env bash

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CSV_FILE="$ROOT_DIR/results/llm-env-results/combined_results.csv"
INSTANCE_FILE="$ROOT_DIR/tsplib/prob.42.sop"
VALIDATOR="$SCRIPT_DIR/validate_precedence"

if [[ ! -f "$CSV_FILE" ]]; then
    echo "NO: combined_results.csv was not found"
    exit 1
fi

if [[ ! -f "$INSTANCE_FILE" ]]; then
    echo "NO: $INSTANCE_FILE was not found"
    exit 1
fi

make -C "$SCRIPT_DIR" >/dev/null || exit 1

tail -n +2 "$CSV_FILE" | while IFS= read -r row || [[ -n "$row" ]]; do
    [[ -z "$row" ]] && continue

    # The model is the first CSV field and Best Path is the final field.
    model="${row%%,*}"
    path="${row##*,}"

    if [[ -z "$path" || "$path" == "-1" ]]; then
        echo "$model: NO"
        continue
    fi

    if "$VALIDATOR" "$INSTANCE_FILE" "$path" >/dev/null 2>&1; then
        echo "$model: YES"
    else
        echo "$model: NO"
    fi
done
