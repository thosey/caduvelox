// Copy to clipboard functionality
document.addEventListener('DOMContentLoaded', function() {
    // Add copy buttons to all code blocks
    const codeBlocks = document.querySelectorAll('.code');
    
    codeBlocks.forEach(block => {
        // Create copy button
        const copyButton = document.createElement('button');
        copyButton.className = 'copy-button';
        copyButton.innerHTML = '📋 Copy';
        copyButton.setAttribute('aria-label', 'Copy code to clipboard');
        
        // Position button in top-right of code block
        block.style.position = 'relative';
        block.appendChild(copyButton);
        
        // Copy functionality
        copyButton.addEventListener('click', async function() {
            try {
                // Get text content, remove HTML tags and clean up
                const text = block.textContent
                    .replace(copyButton.textContent, '') // Remove button text
                    .replace(/\u00A0/g, ' ') // Replace &nbsp; with spaces
                    .trim();
                
                await navigator.clipboard.writeText(text);
                
                // Visual feedback
                copyButton.innerHTML = '✓ Copied!';
                copyButton.classList.add('copied');
                
                // Reset after 2 seconds
                setTimeout(() => {
                    copyButton.innerHTML = '📋 Copy';
                    copyButton.classList.remove('copied');
                }, 2000);
            } catch (err) {
                console.error('Failed to copy:', err);
                copyButton.innerHTML = '✗ Failed';
                setTimeout(() => {
                    copyButton.innerHTML = '📋 Copy';
                }, 2000);
            }
        });
    });
});
