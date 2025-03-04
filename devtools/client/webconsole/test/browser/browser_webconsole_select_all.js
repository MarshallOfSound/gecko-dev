/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test that the global Firefox "Select All" functionality (e.g. Edit >
// Select All) works properly in the Web Console.

/* import-globals-from head.js */

const TEST_URI = "http://example.com/";

add_task(async function testSelectAll() {
  const hud = await openNewTabAndConsole(TEST_URI);
  await testSelectionWhenMovingBetweenBoxes(hud);
  testBrowserMenuSelectAll(hud);
});

async function testSelectionWhenMovingBetweenBoxes(hud) {
  // Fill the console with some output.
  await clearOutput(hud);
  await executeAndWaitForMessage(hud, "1 + 2", "3", ".result");
  await executeAndWaitForMessage(hud, "3 + 4", "7", ".result");
  await executeAndWaitForMessage(hud, "5 + 6", "11", ".result");
}

function testBrowserMenuSelectAll(hud) {
  const { ui } = hud;
  const outputContainer = ui.outputNode.querySelector(".webconsole-output");

  is(
    outputContainer.childNodes.length,
    6,
    "the output node contains the expected number of children"
  );

  // The focus is on the JsTerm, so we need to blur it for the copy comand to
  // work.
  outputContainer.ownerDocument.activeElement.blur();

  // Test that the global Firefox "Select All" functionality (e.g. Edit >
  // Select All) works properly in the Web Console.
  goDoCommand("cmd_selectAll");

  checkMessagesSelected(outputContainer);
  hud.iframeWindow.getSelection().removeAllRanges();
}

function checkMessagesSelected(outputContainer) {
  const selection = outputContainer.ownerDocument.getSelection();
  const messages = outputContainer.querySelectorAll(".message");

  for (const message of messages) {
    const selected = selection.containsNode(message);
    ok(selected, `Node containing text "${message.textContent}" was selected`);
  }
}
