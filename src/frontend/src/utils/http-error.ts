/** An HTTP response error, distinct from transport/validation failures. */
export class HttpError extends Error {
  readonly data: unknown;

  constructor(
    readonly status: number,
    body: string,
  ) {
    super(body);
    this.name = "HttpError";
    try {
      this.data = JSON.parse(body);
    } catch {
      this.data = undefined;
    }
  }
}
