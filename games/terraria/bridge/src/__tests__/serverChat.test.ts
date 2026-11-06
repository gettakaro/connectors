import assert from 'node:assert/strict';
import { test } from 'node:test';
import { renderDirected, renderGlobal, resolveSenderName } from '../terraria/serverChat.js';

test('the override wins, then the config value; blank counts as unset', () => {
  assert.equal(resolveSenderName('Override', 'Config'), 'Override');
  assert.equal(resolveSenderName('  Override  ', 'Config'), 'Override');
  assert.equal(resolveSenderName(undefined, 'Config'), 'Config');
  assert.equal(resolveSenderName('', ' Config '), 'Config');
  assert.equal(resolveSenderName('   ', 'Config'), 'Config');
  assert.equal(resolveSenderName(42, 'Config'), 'Config');
  assert.equal(resolveSenderName(undefined, undefined), null);
  assert.equal(resolveSenderName(' ', '  '), null);
});

test('global chat reads "<name>: <message>", a directed one "<From <name>> <message>"', () => {
  assert.equal(renderGlobal('Limon', 'hello'), 'Limon: hello');
  assert.equal(renderDirected('Limon', 'hello'), '<From Limon> hello');
});
