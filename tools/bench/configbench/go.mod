module magitrickle-configbench

go 1.23.12

require magitrickle v0.0.0

// Historical Go config benchmark: requires a checkout of the original backend.
// Override this replacement with `go mod edit -replace=magitrickle=/path/to/src/backend`.
replace magitrickle => ../../../src/backend
