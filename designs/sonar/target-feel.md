uses Sonar;
uses Std;
uses MyApp::Dialogs;

SonarApp app = SonarApp();
string | None filename;

FlexContainer main = FlexContainer(`
    <contentbar>
        <menu>
            <menuitem id="file-menu" hotkey="^f">File</menuitem>
        </menu>    
    </contentbar>
    <textarea id="text"><textarea>
    <contentbar>
        <span>{{filename}}</span>
    </contentbar>
`);
app.add(main);

FileDialog openDialog = FileDialog('open');
FileDialog saveDialog = FileDialog('save');
app.add(openDialog);
app.add(saveDialog);

FlexContainer fileMenu = FlexContainer(`
    <menu class="hidden">
        <menuitem hotkey="^o">File</menuitem>        
        <menuitem hotkey="^s">Save</menuitem>
        <menuitem hotkey="^+s">Save As</menuitem>
    </menu>    
`);
fileMenu.position.method = PositionMethod::Absolute;
fileMenu.position.x = main.query('#file-menu').position.x;
fileMenu.position.y = main.query('#file-menu').position.y+1;

fineMenu.actions.add('open', () => {
    filename = await openDialog.show();
    using File f = File(filename, std::read);
    main.query(text).value = f.readAll();    
});

fileMenu.actions.add('save', () => {
   using File f = File(path, std::write);
   f.write(main.query(text).value);
   f.close(); 
});

fileMenu.actions.add('save-as', () => {
   if ()
   using File f = File(path, std::write);
   f.write(main.query(text).value);
   f.close(); 
});

app.add(fileMenu);
app.start();