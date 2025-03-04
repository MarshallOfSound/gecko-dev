/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

"use strict";

// This test can be really slow on debug platforms
requestLongerTimeout(5);

add_task(async function test() {
  const dbg = await initDebugger("big-sourcemap.html", "bundle.js");
  invokeInTab("hitDebugStatement");
  await waitForPaused(dbg, "bundle.js");

  await stepIn(dbg);

  const whyPaused = await waitFor(
    () => dbg.win.document.querySelector(".why-paused")?.innerText
  );
  is(whyPaused, `Paused while stepping`);

  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);
  await stepIn(dbg);

  assertDebugLine(dbg, 7679);
  assertPausedLocation(dbg);
});
