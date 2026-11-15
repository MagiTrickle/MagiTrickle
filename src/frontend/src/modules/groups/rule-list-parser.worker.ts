import { RuleListParserSession, type RuleListRequest } from "./rule-list-parser";

const session = new RuleListParserSession();
self.onmessage = (event: MessageEvent<RuleListRequest>) => {
  self.postMessage(session.handle(event.data));
};
