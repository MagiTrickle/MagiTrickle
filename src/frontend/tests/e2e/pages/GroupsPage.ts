import { expect, type Locator, type Page } from "@playwright/test";

export class GroupsPage {
  readonly page: Page;
  readonly saveButton: Locator;
  readonly groupList: Locator;
  readonly addGroupButton: Locator;
  readonly searchContainer: Locator;
  readonly searchInput: Locator;

  constructor(page: Page) {
    this.page = page;
    this.saveButton = page.locator("#save-changes");
    this.groupList = page.locator(".group-list");
    // Select the wrapper that contains the tooltip text "Add Group", then find the button inside it.
    this.addGroupButton = page.locator('[data-value="Add Group"]').locator("button");
    this.searchContainer = page.locator(".search-container");
    this.searchInput = page.locator(".search-input");
  }

  async goto() {
    await this.page.goto("/");
  }

  async search(query: string) {
    await this.searchContainer.click();
    await this.searchInput.fill(query);
  }

  async createGroup() {
    await this.addGroupButton.click();
  }

  async getGroup(index: number) {
    return this.page.locator(`.group-wrapper`).nth(index);
  }

  async getGroupHeader(index: number) {
    return this.page.locator(`.group-header[data-group-index="${index}"]`);
  }

  async setGroupName(index: number, name: string) {
    const header = await this.getGroupHeader(index);
    const input = header.locator("input.group-name");
    await input.fill(name);
  }

  async addRuleToGroup(groupIndex: number) {
    // Find the 'Add Rule' button specifically within the requested group
    // The group header contains the actions.
    const header = await this.getGroupHeader(groupIndex);
    const addRuleBtn = header.locator('[data-value="Add Rule"]').locator("button");
    await addRuleBtn.click();
  }

  async getRule(groupIndex: number, ruleIndex: number) {
    const group = await this.getGroup(groupIndex);
    return group.locator(`.rule[data-index="${ruleIndex}"]`);
  }

  async setRuleName(groupIndex: number, ruleIndex: number, name: string) {
    const rule = await this.getRule(groupIndex, ruleIndex);
    await rule.locator(".name input").fill(name);
  }

  async setRulePattern(groupIndex: number, ruleIndex: number, pattern: string) {
    const rule = await this.getRule(groupIndex, ruleIndex);
    await rule.locator(".pattern input").fill(pattern);
  }

  async setRuleType(groupIndex: number, ruleIndex: number, typeLabel: string) {
    const rule = await this.getRule(groupIndex, ruleIndex);
    const trigger = rule.locator(".type [data-select-trigger]");
    await trigger.click();
    await this.page.getByRole("option", { name: typeLabel }).click();
  }

  async save() {
    await this.saveButton.click();
  }

  async deleteGroup(index: number) {
    const header = await this.getGroupHeader(index);
    const deleteBtn = header.locator('[data-value="Delete Group"]').locator("button");
    await deleteBtn.click();
  }

  async deleteRule(groupIndex: number, ruleIndex: number) {
    const rule = await this.getRule(groupIndex, ruleIndex);
    const deleteBtn = rule.locator('[data-value="Delete Rule"]').locator("button");
    await deleteBtn.click();
  }
}
