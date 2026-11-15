# Plugin contract tests

`backend/mocks` and `frontend/mocks` contain original simulated plugin source. They exercise normal, malformed, failure and legacy-entry contracts without a vendor SDK or device. The backend test runner composes them with the real core/Broker.

Mock source files/projects use Mock names. Test DLL output names and test environment keys retain their existing Fixture spelling so archived callers and assertions remain compatible. All lower-case legacy exports are intentional explicit compatibility cases.
