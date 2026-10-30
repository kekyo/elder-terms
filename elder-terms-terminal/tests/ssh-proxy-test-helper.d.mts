/** SSH forwarding request observed by the test gateway. */
export interface TestSshForwardRequest {
  host: string;
  port: number;
}

/** Gateway details supplied to a fixture using SSH forwarding. */
export interface TestSshGateway {
  port: number;
  requests: TestSshForwardRequest[];
  knownHosts: string;
}

/** Run a gateway that resolves proxy-test.invalid to local test services. */
export const withSshGateway: (
  executable: string,
  directory: string,
  check: (gateway: TestSshGateway) => Promise<void>,
  destination?: string
) => Promise<void>;
