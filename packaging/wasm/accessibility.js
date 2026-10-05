// The browser owns native controls and announces them through its accessibility
// API. The canvas remains the visual surface; every action returns to the shared
// source through Platform::nextAction rather than changing application state here.
Module.gleditorAccessibility = (() => {
  const elements = new Map();
  const descriptions = new Map();
  const pending = [];
  let tree = new Map();
  let surface;
  let focused = true;
  let synchronizing = false;

  const accepts = (node, action) => !!(node.actions & (1 << action));
  function enqueue(node, action, value = '') {
    if (!synchronizing && accepts(node, action)) {
      pending.push({node: node.id, action, value});
    }
  }

  function make(node) {
    const tags = {
      textInput: 'input', passwordInput: 'input', multilineTextInput: 'textarea',
      button: 'button', switch: 'button', link: 'a', list: 'ul',
      listItem: 'li', comboBox: 'select'
    };
    const element = document.createElement(tags[node.role] || 'div');
    element.id = 'gleditor-a11y-' + node.id;
    element.dataset.node = node.id;
    element.dataset.role = node.role;
    if (element.tagName === 'BUTTON') element.type = 'button';
    if (element.tagName === 'INPUT') {
      element.type = node.role === 'passwordInput' ? 'password' : 'text';
      if (node.role === 'passwordInput') {
        element.addEventListener('blur', () => { element.value = ''; });
      }
    }
    if (node.role === 'link') element.href = '#';
    const roles = {
      window: 'group', group: 'group', document: 'document', dialog: 'dialog',
      log: 'log', switch: 'switch'
    };
    if (roles[node.role]) element.setAttribute('role', roles[node.role]);
    element.addEventListener('focus', () => {
      const current = tree.get(node.id);
      if (current) enqueue(current, 0);
    });
    element.addEventListener('click', event => {
      // Nested controls own their actions; a parent document must not receive
      // the same activation again when a child event bubbles.
      if (event.target !== element) return;
      const current = tree.get(node.id);
      if (current && accepts(current, 1)) {
        event.preventDefault();
        event.stopPropagation();
        enqueue(current, 1);
      }
    });
    element.addEventListener('input', event => {
      const current = tree.get(node.id);
      if (current && accepts(current, 2)) {
        event.stopPropagation();
        enqueue(current, 2, element.value);
      }
    });
    element.addEventListener('change', () => {
      const current = tree.get(node.id);
      if (!current || current.role !== 'comboBox') return;
      if (accepts(current, 2)) enqueue(current, 2, element.value);
      else {
        const selected = tree.get(element.value);
        if (selected) enqueue(selected, 1);
      }
    });
    element.addEventListener('beforeinput', event => {
      if (event.target !== element) return;
      const current = tree.get(node.id);
      // Document editing belongs to SDL. Sources that advertise SetValue (form
      // fields) instead use native input and queue the resulting replacement.
      if (current && !accepts(current, 2)) event.preventDefault();
    });
    for (const type of ['keydown', 'keyup', 'keypress']) {
      element.addEventListener(type, event => {
        const current = tree.get(node.id);
        if (!current || event.key === 'Tab') return;
        if (type === 'keydown' && accepts(current, 1) &&
            !['BUTTON', 'A', 'SELECT', 'INPUT', 'TEXTAREA'].includes(element.tagName) &&
            ['Enter', ' '].includes(event.key)) {
          event.preventDefault();
          event.stopPropagation();
          enqueue(current, 1);
          return;
        }
        if (accepts(current, 2) || ['button', 'switch', 'link', 'comboBox'].includes(current.role)) {
          event.stopPropagation();
        }
      });
    }
    return element;
  }

  function textOf(node) {
    if (node.value) return node.value;
    return node.children.map(id => {
      const child = tree.get(id);
      return child ? textOf(child) : '';
    }).join('');
  }

  function selectionOffset(node, point) {
    let offset = 0;
    for (const id of node.children) {
      const child = tree.get(id);
      if (!child) continue;
      if (id === point.node) {
        // Shared offsets count Unicode scalars; HTML selections count UTF-16.
        return offset + Array.from(child.value).slice(0, point.character).join('').length;
      }
      offset += textOf(child).length;
    }
    return Math.min(point.character, textOf(node).length);
  }

  function ensureSurface() {
    if (surface) return;
    surface = document.createElement('div');
    surface.id = 'gleditor-accessibility';
    surface.className = 'gleditor-accessibility';
    document.body.appendChild(surface);
    if (Module.canvas) Module.canvas.setAttribute('aria-hidden', 'true');
  }

  function update(snapshot) {
    ensureSurface();
    synchronizing = true;
    try {
      tree = new Map(snapshot.nodes.map(node => [node.id, node]));
      for (const [id, element] of elements) {
        if (!tree.has(id) || tree.get(id).role !== element.dataset.role) {
          element.remove();
          elements.delete(id);
          descriptions.get(id)?.remove();
          descriptions.delete(id);
        }
      }
      for (const node of snapshot.nodes) {
        let element = elements.get(node.id);
        if (!element) {
          element = make(node);
          elements.set(node.id, element);
        }
        element.inert = false;
        element.removeAttribute('aria-hidden');
        element.setAttribute('aria-label', node.label);
        element.tabIndex = node.focusable || accepts(node, 0) || accepts(node, 1) ? 0 : -1;
        if (node.description) {
          let description = descriptions.get(node.id);
          if (!description) {
            description = document.createElement('span');
            description.id = element.id + '-description';
            description.hidden = true;
            surface.appendChild(description);
            descriptions.set(node.id, description);
          }
          description.textContent = node.description;
          element.setAttribute('aria-describedby', description.id);
        } else {
          element.removeAttribute('aria-describedby');
          descriptions.get(node.id)?.remove();
          descriptions.delete(node.id);
        }
        element.setAttribute('aria-live', ['off', 'polite', 'assertive'][node.live]);
        if ('toggled' in node) element.setAttribute('aria-checked', String(node.toggled));
        else element.removeAttribute('aria-checked');
        if (node.modal) element.setAttribute('aria-modal', 'true');
        else element.removeAttribute('aria-modal');
        if (element.tagName === 'INPUT' || element.tagName === 'TEXTAREA') {
          if (node.role === 'passwordInput') {
            // The shared tree contains masking characters, never the secret.
            // A native edit is a whole replacement; importing those masks
            // would turn a subsequent keystroke into literal stars + text.
            if (document.activeElement !== element) element.value = '';
            element.placeholder = node.placeholder ||
              (node.value ? 'Password already set; type to replace' : '');
          } else {
            const value = textOf(node);
            if (element.value !== value) element.value = value;
            element.placeholder = node.placeholder;
          }
          element.readOnly = node.readOnly;
          if (node.selection && element.type !== 'password') {
            const anchor = selectionOffset(node, node.selection.anchor);
            const focus = selectionOffset(node, node.selection.focus);
            element.setSelectionRange(Math.min(anchor, focus), Math.max(anchor, focus),
                                      focus < anchor ? 'backward' : 'forward');
          }
        } else if (element.tagName !== 'SELECT') {
          const ownText = node.role === 'label' ? [node.label, node.value].filter(Boolean).join(' ')
            : node.value || (['button', 'switch', 'link', 'listItem'].includes(node.role) ? node.label : '');
          let text = element.firstChild;
          if (!text || text.nodeType !== Node.TEXT_NODE) {
            text = document.createTextNode('');
            element.prepend(text);
          }
          if (text.data !== ownText) text.data = ownText;
        }
      }

      // Reuse nodes and leave unchanged relationships in place: detaching an
      // active native input would discard focus and the browser's selection.
      for (const node of snapshot.nodes) {
        const parent = elements.get(node.id);
        if (['INPUT', 'TEXTAREA'].includes(parent.tagName)) continue;
        if (parent.tagName === 'SELECT') {
          const wanted = new Set(node.children);
          for (const option of Array.from(parent.options)) {
            if (!wanted.has(option.value)) option.remove();
          }
          for (const id of node.children) {
            const child = tree.get(id);
            if (!child) continue;
            let option = Array.from(parent.options).find(candidate => candidate.value === id);
            if (!option) {
              option = document.createElement('option');
              option.value = id;
              parent.appendChild(option);
            }
            option.textContent = child.label || child.value;
            option.selected = node.value === child.value || node.value === child.label || node.value === id;
            elements.get(id).remove();
          }
          continue;
        }
        let previous = parent.firstChild;
        for (const id of node.children) {
          const child = elements.get(id);
          if (!child) continue;
          const next = previous?.nextSibling;
          if (child.parentNode !== parent || child !== next) parent.insertBefore(child, next || null);
          previous = child;
        }
      }
      const root = elements.get(snapshot.nodes[0]?.id);
      if (root && root.parentNode !== surface) surface.appendChild(root);
      const modal = snapshot.nodes.findLast(node => node.modal);
      if (modal) {
        let current = elements.get(modal.id);
        while (current && current !== surface) {
          for (const sibling of current.parentNode?.children || []) {
            if (sibling !== current) {
              sibling.inert = true;
              sibling.setAttribute('aria-hidden', 'true');
            }
          }
          current = current.parentNode;
        }
      }
      const target = elements.get(snapshot.focus);
      if (focused && target && !target.closest('[inert]') && document.activeElement !== target) {
        target.focus({preventScroll: true});
      }
    } finally {
      synchronizing = false;
    }
  }

  return {
    update,
    setFocused(value) { focused = value; },
    nextAction() {
      while (pending.length) {
        const request = pending.shift();
        const node = tree.get(request.node);
        if (node && accepts(node, request.action)) return request;
      }
      return null;
    },
    close() {
      surface?.remove();
      surface = undefined;
      elements.clear();
      descriptions.clear();
      tree.clear();
      pending.length = 0;
    }
  };
})();
