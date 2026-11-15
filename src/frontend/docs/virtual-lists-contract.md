# Virtual collection UI contract

Groups and Subscriptions are the visual reference. Virtualization changes which
panels and rules are mounted, not their data, appearance or actions. This document
records the behavior to retain while replacing pagination and staged rendering.

## Geometry and layering

- Preserve the page toolbar, panel header, editable title, actions, stripes and
  column alignment. Header inset is 0.5rem; it is not a page gutter.
- Group rule rows use the existing `1.1rem 2.5fr 1fr 3fr 1fr` desktop grid,
  0.5rem gap and 0.1rem padding. Subscription rows use
  `2.5rem 5.5fr 1fr 0.6fr`, 0.5rem gap and 0.1rem vertical padding.
- At 700px and below both row types become stacked cards. Their heights can
  differ from desktop and change with wrapping. Resize and font loading must
  update measurements rather than leaving gaps or overlapping rows.
- Use native document scrolling with the original overlay scrollbar restored
  from before `a139e6d`, retaining its header bounds, appearance, auto-hide and
  pointer capture. Wheel and keyboard scrolling remain browser-owned; only an
  explicit scrollbar drag/click writes the document position. The overlay does
  not reserve a page gutter or shift content when a dialog opens. Virtual spacers do not
  clip menus, introduce stacking contexts above portals or cover neighboring
  panels. Popup hit testing must succeed where a popup overlaps another row.
- Back to top reaches document offset zero even while virtual measurements
  change. Repeated clicks work; wheel, touch, pointer and navigation keys cancel
  the animation. Reduced motion jumps directly to the top.
- Home/End keep their native browser behavior. Ctrl/Cmd+Up/Down and Back to top
  initiate a native smooth scroll, or an instant scroll with reduced motion.
  After scrollend and the destination panel's layout frames, check the requested
  edge again: remounting an expanded panel can change the document height or
  clamp Safari's original target. Continue natively only if that edge was lost;
  release the request on arrival or cancellation. Do not implement easing,
  replay the destination each frame or write scroll offsets from ResizeObserver:
  those writes interrupt the browser's animation. Manual
  input cancels an initiated move. Editable fields, selections, menus and dialogs
  keep their native keyboard behavior. Do not prevent or replay wheel deltas to
  override the browser's native handoff from a cancelled animation.
- Keep native scroll anchoring available on real rows; exclude only spacers.
  Learn common rule and collapsed-panel sizes from a consistent visible batch,
  while retaining individual measurements for wrapping or other exceptions.
  Preserve fractional CSS pixels in both measurements and common estimates;
  rounding each row makes the document height depend on the mounted range.
  Very tall spacers keep their fractional part separate from their whole-pixel
  height so CSS length precision does not change the sum as rows mount.
  Shared estimates survive panel unmounts and have separate mobile/desktop values.
  The collection owns cached rule measurements through a weak key; resizing
  invalidates incompatible geometry without retaining deleted collections.
- Functional panel estimates track every panel's open state and rule count,
  including offscreen panels. Changing one panel must not clear unrelated
  measurements. Updating a numeric estimate preserves already measured rows.
- A tooltip or open popup must never remain orphaned when its trigger unmounts.
  Escape closes a popup and restores its trigger when that trigger remains
  mounted. Focused editors and active drag sources need stable ownership.
- Rule components may reuse inactive DOM slots. Keep logical IDs on the rows,
  reset transient state when a slot changes identity and remeasure its height.
  Never recycle a focused editor, open popup anchor or active drag source.
  Panel components retain their logical keys because they own editor state.
- Share document viewport scheduling across lists. Repeated scroll/resize
  notifications with unchanged ranges or sizes must not rebuild the layout.
  Viewport reads precede virtual range commits; the browser owns scrolling.
- Group and subscription rules expand and collapse immediately, with no height
  or opacity transition. Closed panels retain their accessibility relationships without
  measuring or mounting offscreen rule content.
- Local popup and preview scrolling uses the same VirtualList with its own
  scroll element. It must not move the document, clip portals or grow mounted
  rows with the complete result count.

## Data and interactions

- All imported data becomes the draft together; readiness and import success do
  not wait for offscreen DOM. Export and Save include every item, including
  unmounted rules. Import still validates and plans conflicts before staging.
- Edits, enabled state, collapse state, sorting and selections belong to their
  existing owners and survive scroll away/back and switching tabs. Virtual row
  identity uses stable IDs; row numbers use current original positions.
- Search examines all data and renders matches, including formerly unmounted
  groups and rules. Clearing search restores the full collection. Duplicate
  navigation expands and scrolls to the requested rule before focusing it.
- Explicit Add focuses the newly added name/pattern. Automatic mount/scroll
  never steals focus. Keyboard navigation must not silently stop at the last
  currently mounted row.
- Group/rule and subscription drag order, cross-group destinations, empty-header
  drop, before/after edges and auto-scroll retain their existing meaning.
- Virtualization does not alter compact PUT payloads, canonical server IDs,
  persistence retries, validation, error handling or unsaved draft protection.

## Import preparation

- Keep immutable snapshots in one worker session while the import dialog is
  open. Selection changes send lightweight messages; previews contain metadata,
  not complete rule arrays. Request the full plan only when applying it.
- Match replies to both session and selection version. Apply stays unavailable
  until the latest selection is ready. Closing the dialog terminates its worker;
  reopening cannot reuse stale plans or copy IDs from the previous session.
- Build detached change tracking, validation and duplicate indexes in bounded
  pieces that yield to browser tasks, including inside one large rule array.
  Preparation must not publish partial drafts or invalidate background loads.
  Reuse the preparation's own detached nodes instead of cloning them again;
  saved baselines and caller-owned snapshots must remain separate.
- Validate every prepared store and the current editor snapshot before committing
  the transaction synchronously. Reject stale preparation without losing edits.
  Imported drafts remain reversible and Save retains its existing API contract.

## Native rule-list editor

- Parsing, type detection, statistics, deduplication and ID generation run in
  a cancellable worker session. The complete rule list stays there until Apply;
  the preview is limited to the first 500 lines and 32 Ki UTF-16 text units,
  retaining a continuation marker and the full original editor buffer.
- Apply pulls one bounded worker message at a time (up to 256 fragments and
  32 Ki UTF-16 text units). Assemble a private result array without repeated
  concatenation, and publish it only after every ordered chunk is complete.
  Cancellation discards the partial result; late replies from a terminated
  session cannot affect a later import. Parser failure invalidates the preview
  cache so retry reparses the retained input in a new worker.
- Preview and edit modes retain one textarea and its native editing buffer.
  Technical text has spellcheck, correction and writing suggestions disabled.
  Resizing reads only enough lines to reach the editor's maximum height.
  Closing clears the bulk text only after the textarea leaves the DOM, including
  a dialog outro. Reopening during that outro starts a fresh editor without a
  delayed reset of the new input.
- Type or text changes invalidate cached results, including while editing.
  Cancel, close and destruction terminate obsolete work. Apply awaits one
  atomic store commit; cancellation and errors keep the previous draft intact
  and errors leave the entered text available for retry.
- Imported rule IDs must be unique against existing groups/rules and within the
  batch. Bulk insertion and sort preserve tracked array identity without passing
  an unbounded argument list to JavaScript array methods.
- Duplicate indicators share an index. Detailed conflict lists are built only
  for open menus, use bounded caches and virtual DOM, and background rebuilds
  yield and discard stale results after another edit.

## Validation boundaries

Use desktop 1280×900 and mobile 390×844, plus resize and long content. Check
100,000-rule data and many short panels: mounted DOM must remain bounded while
scrolling to the final item, editing, searching and exporting the complete draft.
Measure any performance claim with the actual test configuration and browser;
bounded DOM is a structural assertion, not a latency benchmark.

Browser tests use API mocks and do not prove router persistence or routing.
Retain the existing popover, DnD, duplicate navigation, accessibility and save
suites alongside the virtual-list regressions. Update pagination-only
expectations narrowly because pagination is intentionally removed.
