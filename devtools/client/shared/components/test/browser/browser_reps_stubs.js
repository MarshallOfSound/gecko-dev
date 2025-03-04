/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/* import-globals-from ../../../../shared/test/shared-head.js */

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/shared/test/shared-head.js",
  this
);

const TEST_URI = "data:text/html;charset=utf-8,stub generation";
/**
 * A Map keyed by filename, and for which the value is also a Map, with the key being the
 * label for the stub, and the value the expression to evaluate to get the stub.
 */
const EXPRESSIONS_BY_FILE = {
  "attribute.js": new Map([
    [
      "Attribute",
      `{
          const a = document.createAttribute("class")
          a.value = "autocomplete-suggestions";
          a;
      }`,
    ],
  ]),
  "infinity.js": new Map([
    ["Infinity", `Infinity`],
    ["NegativeInfinity", `-Infinity`],
  ]),
  "nan.js": new Map([["NaN", `2 * document`]]),
  "null.js": new Map([["Null", `null`]]),
  "number.js": new Map([
    ["Int", `2 + 3`],
    ["True", `true`],
    ["False", `false`],
    ["NegZeroGrip", `1 / -Infinity`],
  ]),
  "undefined.js": new Map([["Undefined", `undefined`]]),
  // XXX: File a bug blocking Bug 1671400 for enabling automatic generation for one of
  // the following file.
  // "accessible.js",
  // "accessor.js",
  // "big-int.js",
  // "comment-node.js",
  // "date-time.js",
  // "document-type.js",
  // "document.js",
  // "element-node.js",
  // "error.js",
  // "event.js",
  // "failure.js",
  // "function.js",
  // "grip-array.js",
  // "grip-map-entry.js",
  // "grip-map.js",
  // "grip.js",
  // "long-string.js",
  // "object-with-text.js",
  // "object-with-url.js",
  // "promise.js",
  // "regexp.js",
  // "stylesheet.js",
  // "symbol.js",
  // "text-node.js",
  // "window.js",
};

add_task(async function() {
  const isStubsUpdate = env.get(STUBS_UPDATE_ENV) == "true";

  const tab = await addTab(TEST_URI);
  const {
    CommandsFactory,
  } = require("devtools/shared/commands/commands-factory");
  const commands = await CommandsFactory.forTab(tab);
  await commands.targetCommand.startListening();

  let failed = false;
  for (const stubFile of Object.keys(EXPRESSIONS_BY_FILE)) {
    info(`${isStubsUpdate ? "Update" : "Check"} ${stubFile}`);

    const generatedStubs = await generateStubs(commands, stubFile);
    if (isStubsUpdate) {
      await writeStubsToFile(env, stubFile, generatedStubs);
      ok(true, `${stubFile} was updated`);
      continue;
    }

    const existingStubs = getStubFile(stubFile);
    if (generatedStubs.size !== existingStubs.size) {
      failed = true;
      continue;
    }

    for (const [key, packet] of generatedStubs) {
      const packetStr = getSerializedPacket(packet, {
        sortKeys: true,
        replaceActorIds: true,
      });
      const grip = getSerializedPacket(existingStubs.get(key), {
        sortKeys: true,
        replaceActorIds: true,
      });
      is(packetStr, grip, `"${key}" packet has expected value`);
      failed = failed || packetStr !== grip;
    }
  }

  if (failed) {
    ok(
      false,
      "The reps stubs need to be updated by running `" +
        `mach test ${getCurrentTestFilePath()} --headless --setenv STUBS_UPDATE=true` +
        "`"
    );
  } else {
    ok(true, "Stubs are up to date");
  }

  await removeTab(tab);
});

async function generateStubs(commands, stubFile) {
  const stubs = new Map();

  for (const [key, expression] of EXPRESSIONS_BY_FILE[stubFile]) {
    const { result } = await commands.scriptCommand.execute(expression);
    stubs.set(key, getCleanedPacket(stubFile, key, result));
  }

  return stubs;
}

function getCleanedPacket(stubFile, key, packet) {
  // Remove the targetFront property that has a cyclical reference and that we don't need
  // in our node tests.
  delete packet.targetFront;

  const existingStubs = getStubFile(stubFile);
  if (!existingStubs) {
    return packet;
  }

  // Strip escaped characters.
  const safeKey = key
    .replace(/\\n/g, "\n")
    .replace(/\\r/g, "\r")
    .replace(/\\\"/g, `\"`)
    .replace(/\\\'/g, `\'`);
  if (!existingStubs.has(safeKey)) {
    return packet;
  }

  // If the stub already exist, we want to ignore irrelevant properties (generated id, timer, …)
  // that might changed and "pollute" the diff resulting from this stub generation.
  const existingPacket = existingStubs.get(safeKey);

  // copy existing contentDomReference
  if (
    packet._grip?.contentDomReference?.id &&
    existingPacket._grip?.contentDomReference?.id
  ) {
    packet._grip.contentDomReference = existingPacket._grip.contentDomReference;
  }

  return packet;
}

// HELPER

const CHROME_PREFIX = "chrome://mochitests/content/browser/";
const STUBS_FOLDER = "devtools/client/shared/components/test/node/stubs/reps/";
const STUBS_UPDATE_ENV = "STUBS_UPDATE";

/**
 * Write stubs to a given file
 *
 * @param {Object} env
 * @param {String} fileName: The file to write the stubs in.
 * @param {Map} packets: A Map of the packets.
 */
async function writeStubsToFile(env, fileName, packets) {
  const mozRepo = env.get("MOZ_DEVELOPER_REPO_DIR");
  const filePath = `${mozRepo}/${STUBS_FOLDER + fileName}`;

  const stubs = Array.from(packets.entries()).map(([key, packet]) => {
    const stringifiedPacket = getSerializedPacket(packet);
    return `stubs.set(\`${key}\`, ${stringifiedPacket});`;
  });

  const fileContent = `/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

"use strict";
/*
 * THIS FILE IS AUTOGENERATED. DO NOT MODIFY BY HAND. RUN browser_reps_stubs.js with STUBS_UPDATE=true env TO UPDATE.
 */

const stubs = new Map();
${stubs.join("\n\n")}

module.exports = stubs;
`;

  const textEncoder = new TextEncoder();
  await IOUtils.write(filePath, textEncoder.encode(fileContent));
}

function getStubFile(fileName) {
  return require(CHROME_PREFIX + STUBS_FOLDER + fileName);
}

function sortObjectKeys(obj) {
  const isArray = Array.isArray(obj);
  const isObject = Object.prototype.toString.call(obj) === "[object Object]";
  const isFront = obj?._grip;

  if (isObject && !isFront) {
    // Reorder keys for objects, but skip fronts to avoid infinite recursion.
    const sortedKeys = Object.keys(obj).sort((k1, k2) => k1.localeCompare(k2));
    const withSortedKeys = {};
    sortedKeys.forEach(k => {
      withSortedKeys[k] = k !== "stacktrace" ? sortObjectKeys(obj[k]) : obj[k];
    });
    return withSortedKeys;
  } else if (isArray) {
    return obj.map(item => sortObjectKeys(item));
  }
  return obj;
}

/**
 * @param {Object} packet
 *        The packet to serialize.
 * @param {Object} options
 * @param {Boolean} options.sortKeys
 *        Pass true to sort all keys alphabetically in the packet before serialization.
 *        For instance stub comparison should not fail if the order of properties changed.
 * @param {Boolean} options.replaceActorIds
 *        Pass true to replace actorIDs with a fake one so it's easier to compare stubs
 *        that includes grips.
 */
function getSerializedPacket(
  packet,
  { sortKeys = false, replaceActorIds = false } = {}
) {
  if (sortKeys) {
    packet = sortObjectKeys(packet);
  }

  const actorIdPlaceholder = "XXX";

  return JSON.stringify(
    packet,
    function(key, value) {
      // The message can have fronts that we need to serialize
      if (value && value._grip) {
        return {
          _grip: value._grip,
          actorID: replaceActorIds ? actorIdPlaceholder : value.actorID,
        };
      }

      if (
        replaceActorIds &&
        (key === "actor" || key === "actorID" || key === "sourceId") &&
        typeof value === "string"
      ) {
        return actorIdPlaceholder;
      }

      return value;
    },
    2
  );
}
