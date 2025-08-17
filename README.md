# Envoy ring cache filter

...

## Building

To build the Envoy static binary:

1. `git submodule update --init`
2. `bazel build //:envoy`

## Running

To run the Envoy binary with the ring cache filter using the [`cache_filter.yaml`](cache_filter.yaml) configuration file:  
`./bazel-bin/envoy -c cache_filter.yaml`

## Testing

To run the unit tests for the ring cache filter:  
`bazel test //ring-cache/test:unit_test`

To run the integration tests:  
`bazel test //ring-cache/test:integration_test`

## How it works

The [Envoy repository](https://github.com/envoyproxy/envoy/) is provided as a submodule.
The [`WORKSPACE`](WORKSPACE) file maps the `@envoy` repository to this local path.

The [`BUILD`](BUILD) file introduces a new Envoy static binary target, `envoy`,
that links together the new filter and `@envoy//source/exe:envoy_main_entry_lib`. The
`ring_cache` filter registers itself during the static initialization phase of the
Envoy binary as a new filter.
