.PHONY: build test

# Keep the established workspace build and full test suites behind one entry.
build:
	npm run build --workspaces

test:
	npm test --workspaces
